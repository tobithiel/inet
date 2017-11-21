//
// Copyright (C) 2010 Helene Lageber
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

#include "inet/routing/bgpv4/BGPRouting.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/routing/ospfv2/OSPFRouting.h"
#include "inet/routing/bgpv4/BGPSession.h"

namespace inet {

namespace bgp {

Define_Module(BgpRouting);

BgpRouting::~BgpRouting(void)
{
    for (auto & elem : _BGPSessions)
    {
        delete (elem).second;
    }

    for (auto & elem : _prefixListINOUT) {
        delete (elem);
    }
}

void BgpRouting::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_ROUTING_PROTOCOLS) {
        bool isOperational;
        NodeStatus *nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
        isOperational = (!nodeStatus) || nodeStatus->getState() == NodeStatus::UP;
        if (!isOperational)
            throw cRuntimeError("This module doesn't support starting in node DOWN state");

        // we must wait until IPv4RoutingTable is completely initialized
        _rt = getModuleFromPar<IIpv4RoutingTable>(par("routingTableModule"), this);
        _inft = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);

        // read BGP configuration
        cXMLElement *bgpConfig = par("bgpConfig").xmlValue();
        loadConfigFromXML(bgpConfig);
        createWatch("myAutonomousSystem", _myAS);
        WATCH_PTRVECTOR(_BGPRoutingTable);
    }
}

void BgpRouting::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()) {    //BGP level
        handleTimer(msg);
    }
    else if (!strcmp(msg->getArrivalGate()->getName(), "socketIn")) {    //TCP level
        processMessageFromTCP(msg);
    }
    else {
        delete msg;
    }
}

void BgpRouting::handleTimer(cMessage *timer)
{
    BgpSession *pSession = (BgpSession *)timer->getContextPointer();
    if (pSession) {
        switch (timer->getKind()) {
            case START_EVENT_KIND:
                EV_INFO << "Processing Start Event" << std::endl;
                pSession->getFSM()->ManualStart();
                break;

            case CONNECT_RETRY_KIND:
                EV_INFO << "Expiring Connect Retry Timer" << std::endl;
                pSession->getFSM()->ConnectRetryTimer_Expires();
                break;

            case HOLD_TIME_KIND:
                EV_INFO << "Expiring Hold Timer" << std::endl;
                pSession->getFSM()->HoldTimer_Expires();
                break;

            case KEEP_ALIVE_KIND:
                EV_INFO << "Expiring Keep Alive timer" << std::endl;
                pSession->getFSM()->KeepaliveTimer_Expires();
                break;

            default:
                throw cRuntimeError("Invalid timer kind %d", timer->getKind());
        }
    }
}

bool BgpRouting::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    throw cRuntimeError("Lifecycle operation support not implemented");
}

void BgpRouting::finish()
{
    unsigned int statTab[NB_STATS] = {
        0, 0, 0, 0, 0, 0
    };
    for (auto & elem : _BGPSessions) {
        (elem).second->getStatistics(statTab);
    }
    recordScalar("OPENMsgSent", statTab[0]);
    recordScalar("OPENMsgRecv", statTab[1]);
    recordScalar("KeepAliveMsgSent", statTab[2]);
    recordScalar("KeepAliveMsgRcv", statTab[3]);
    recordScalar("UpdateMsgSent", statTab[4]);
    recordScalar("UpdateMsgRcv", statTab[5]);
}

void BgpRouting::listenConnectionFromPeer(SessionId sessionID)
{
    if (_BGPSessions[sessionID]->getSocketListen()->getState() == TcpSocket::CLOSED) {
        //session StartDelayTime error, it's anormal that listenSocket is closed.
        _socketMap.removeSocket(_BGPSessions[sessionID]->getSocketListen());
        _BGPSessions[sessionID]->getSocketListen()->abort();
        _BGPSessions[sessionID]->getSocketListen()->renewSocket();
    }
    if (_BGPSessions[sessionID]->getSocketListen()->getState() != TcpSocket::LISTENING) {
        _BGPSessions[sessionID]->getSocketListen()->setOutputGate(gate("socketOut"));
        _BGPSessions[sessionID]->getSocketListen()->bind(TCP_PORT);
        _BGPSessions[sessionID]->getSocketListen()->listen();
        _socketMap.addSocket(_BGPSessions[sessionID]->getSocketListen());
    }
}

void BgpRouting::openTCPConnectionToPeer(SessionId sessionID)
{
    InterfaceEntry *intfEntry = _BGPSessions[sessionID]->getLinkIntf();
    TcpSocket *socket = _BGPSessions[sessionID]->getSocket();
    if (socket->getState() != TcpSocket::NOT_BOUND) {
        _socketMap.removeSocket(socket);
        socket->abort();
        socket->renewSocket();
    }
    socket->setCallbackObject(this, (void *)(uintptr_t)sessionID);
    socket->setOutputGate(gate("socketOut"));
    socket->bind(intfEntry->ipv4Data()->getIPAddress(), 0);
    _socketMap.addSocket(socket);

    socket->connect(_BGPSessions[sessionID]->getPeerAddr(), TCP_PORT);
}

void BgpRouting::processMessageFromTCP(cMessage *msg)
{
    TcpSocket *socket = _socketMap.findSocketFor(msg);
    if (!socket) {
        socket = new TcpSocket(msg);
        socket->setOutputGate(gate("socketOut"));
        Ipv4Address peerAddr = socket->getRemoteAddress().toIPv4();
        SessionId i = findIdFromPeerAddr(_BGPSessions, peerAddr);
        if (i == (SessionId)-1) {
            socket->close();
            delete socket;
            delete msg;
            return;
        }
        socket->setCallbackObject(this, (void *)(uintptr_t)i);

        _socketMap.addSocket(socket);
        _BGPSessions[i]->getSocket()->abort();
        _BGPSessions[i]->setSocket(socket);
    }

    socket->processMessage(msg);
}

void BgpRouting::socketEstablished(int connId, void *yourPtr)
{
    _currSessionId = findIdFromSocketConnId(_BGPSessions, connId);
    if (_currSessionId == (SessionId)-1) {
        throw cRuntimeError("socket id=%d is not established", connId);
    }

    //if it's an IGP Session, TCPConnectionConfirmed only if all EGP Sessions established
    if (_BGPSessions[_currSessionId]->getType() == IGP &&
        this->findNextSession(EGP) != (SessionId)-1)
    {
        _BGPSessions[_currSessionId]->getFSM()->TcpConnectionFails();
    }
    else {
        _BGPSessions[_currSessionId]->getFSM()->TcpConnectionConfirmed();
        _BGPSessions[_currSessionId]->getSocketListen()->abort();
    }

    if (_BGPSessions[_currSessionId]->getSocketListen()->getConnectionId() != connId &&
        _BGPSessions[_currSessionId]->getType() == EGP &&
        this->findNextSession(EGP) != (SessionId)-1)
    {
        _BGPSessions[_currSessionId]->getSocketListen()->abort();
    }
}

void BgpRouting::socketDataArrived(int connId, void *yourPtr, Packet *msg, bool urgent)
{
    _currSessionId = findIdFromSocketConnId(_BGPSessions, connId);
    if (_currSessionId != (SessionId)-1) {
        //TODO: should queuing incoming payloads, and peek from the queue
        const auto& ptrHdr = msg->peekHeader<BgpHeader>();
        switch (ptrHdr->getType()) {
            case BGP_OPEN:
                //BGPOpenMessage* ptrMsg = check_and_cast<BGPOpenMessage*>(msg);
                processMessage(*check_and_cast<const BgpOpenMessage *>(ptrHdr.get()));
                break;

            case BGP_KEEPALIVE:
                processMessage(*check_and_cast<const BgpKeepAliveMessage *>(ptrHdr.get()));
                break;

            case BGP_UPDATE:
                processMessage(*check_and_cast<const BgpUpdateMessage *>(ptrHdr.get()));
                break;

            default:
                throw cRuntimeError("Invalid BGP message type %d", ptrHdr->getType());
        }
    }
    delete msg;
}

void BgpRouting::socketFailure(int connId, void *yourPtr, int code)
{
    _currSessionId = findIdFromSocketConnId(_BGPSessions, connId);
    if (_currSessionId != (SessionId)-1) {
        _BGPSessions[_currSessionId]->getFSM()->TcpConnectionFails();
    }
}

void BgpRouting::processMessage(const BgpOpenMessage& msg)
{
    EV_INFO << "Processing BGP OPEN message" << std::endl;
    _BGPSessions[_currSessionId]->getFSM()->OpenMsgEvent();
}

void BgpRouting::processMessage(const BgpKeepAliveMessage& msg)
{
    EV_INFO << "Processing BGP Keep Alive message" << std::endl;
    _BGPSessions[_currSessionId]->getFSM()->KeepAliveMsgEvent();
}

void BgpRouting::processMessage(const BgpUpdateMessage& msg)
{
    EV_INFO << "Processing BGP Update message" << std::endl;
    _BGPSessions[_currSessionId]->getFSM()->UpdateMsgEvent();

    unsigned char decisionProcessResult;
    Ipv4Address netMask(Ipv4Address::ALLONES_ADDRESS);
    RoutingTableEntry *entry = new RoutingTableEntry();
    const unsigned char length = msg.getNLRI().length;
    unsigned int ASValueCount = msg.getPathAttributeList(0).getAsPath(0).getValue(0).getAsValueArraySize();

    entry->setDestination(msg.getNLRI().prefix);
    netMask = Ipv4Address::makeNetmask(length);
    entry->setNetmask(netMask);
    for (unsigned int j = 0; j < ASValueCount; j++) {
        entry->addAS(msg.getPathAttributeList(0).getAsPath(0).getValue(0).getAsValue(j));
    }

    decisionProcessResult = asLoopDetection(entry, _myAS);

    if (decisionProcessResult == ASLOOP_NO_DETECTED) {
        // RFC 4271, 9.1.  Decision Process
        decisionProcessResult = decisionProcess(msg, entry, _currSessionId);
        //RFC 4271, 9.2.  Update-Send Process
        if (decisionProcessResult != 0)
            updateSendProcess(decisionProcessResult, _currSessionId, entry);
    }
    else
        delete entry;
}

/* add entry to routing table, or delete entry */
unsigned char BgpRouting::decisionProcess(const BgpUpdateMessage& msg, RoutingTableEntry *entry, SessionId sessionIndex)
{
    //Don't add the route if it exists in PrefixListINTable or in ASListINTable
    if (isInTable(_prefixListIN, entry) != (unsigned long)-1 || isInASList(_ASListIN, entry)) {
        delete entry;
        return 0;
    }

    /*If the AS_PATH attribute of a BGP route contains an AS loop, the BGP
       route should be excluded from the decision process. */
    entry->setPathType(msg.getPathAttributeList(0).getOrigin().getValue());
    entry->setGateway(msg.getPathAttributeList(0).getNextHop().getValue());

    //if the route already exist in BGP routing table, tieBreakingProcess();
    //(RFC 4271: 9.1.2.2 Breaking Ties)
    unsigned long BGPindex = isInTable(_BGPRoutingTable, entry);
    if (BGPindex != (unsigned long)-1) {
        if (tieBreakingProcess(_BGPRoutingTable[BGPindex], entry)) {
            delete entry;
            return 0;
        }
        else {
            entry->setInterface(_BGPSessions[sessionIndex]->getLinkIntf());
            _BGPRoutingTable.push_back(entry);
            _rt->addRoute(entry);
            return ROUTE_DESTINATION_CHANGED;
        }
    }

    //Don't add the route if it exists in IPv4 routing table except if the msg come from IGP session
    int indexIP = isInRoutingTable(_rt, entry->getDestination());
    if (indexIP != -1 && _rt->getRoute(indexIP)->getSourceType() != IRoute::BGP) {
        if (_BGPSessions[sessionIndex]->getType() != IGP) {
            delete entry;
            return 0;
        }
        else {
            Ipv4Route *oldEntry = _rt->getRoute(indexIP);
            Ipv4Route *newEntry = new Ipv4Route;
            newEntry->setDestination(oldEntry->getDestination());
            newEntry->setNetmask(oldEntry->getNetmask());
            newEntry->setGateway(oldEntry->getGateway());
            newEntry->setInterface(oldEntry->getInterface());
            newEntry->setSourceType(IRoute::BGP);
            _rt->deleteRoute(oldEntry);
            _rt->addRoute(newEntry);
            //FIXME model error: the RoutingTableEntry *entry will be stored in _BGPRoutingTable, but not stored in _rt, memory leak
            //FIXME model error: The entry inserted to _BGPRoutingTable, but newEntry inserted to _rt; entry and newEntry are differ.
        }
    }

    entry->setInterface(_BGPSessions[sessionIndex]->getLinkIntf());
    _BGPRoutingTable.push_back(entry);

    if (_BGPSessions[sessionIndex]->getType() == EGP) {
        std::string entryh = entry->getDestination().str();
        std::string entryn = entry->getNetmask().str();
        _rt->addRoute(entry);
        //insertExternalRoute on OSPF ExternalRoutingTable if OSPF exist on this BGP router
        if (ospfExist(_rt)) {
            ospf::Ipv4AddressRange OSPFnetAddr;
            OSPFnetAddr.address = entry->getDestination();
            OSPFnetAddr.mask = entry->getNetmask();
            ospf::OspfRouting *ospf = getModuleFromPar<ospf::OspfRouting>(par("ospfRoutingModule"), this);
            InterfaceEntry *ie = entry->getInterface();
            if (!ie)
                throw cRuntimeError("Model error: interface entry is nullptr");
            ospf->insertExternalRoute(ie->getInterfaceId(), OSPFnetAddr);
        }
    }
    return NEW_ROUTE_ADDED;     //FIXME model error: When returns NEW_ROUTE_ADDED then entry stored in _BGPRoutingTable, but sometimes not stored in _rt
}

bool BgpRouting::tieBreakingProcess(RoutingTableEntry *oldEntry, RoutingTableEntry *entry)
{
    /*a) Remove from consideration all routes that are not tied for
         having the smallest number of AS numbers present in their
         AS_PATH attributes.*/
    if (entry->getASCount() < oldEntry->getASCount()) {
        deleteBGPRoutingEntry(oldEntry);
        return false;
    }

    /* b) Remove from consideration all routes that are not tied for
         having the lowest Origin number in their Origin attribute.*/
    if (entry->getPathType() < oldEntry->getPathType()) {
        deleteBGPRoutingEntry(oldEntry);
        return false;
    }
    return true;
}

void BgpRouting::updateSendProcess(const unsigned char type, SessionId sessionIndex, RoutingTableEntry *entry)
{
    //Don't send the update Message if the route exists in listOUTTable
    //SESSION = EGP : send an update message to all BGP Peer (EGP && IGP)
    //if it is not the currentSession and if the session is already established
    //SESSION = IGP : send an update message to External BGP Peer (EGP) only
    //if it is not the currentSession and if the session is already established
    for (auto & elem : _BGPSessions)
    {
        if (isInTable(_prefixListOUT, entry) != (unsigned long)-1 || isInASList(_ASListOUT, entry) ||
            ((elem).first == sessionIndex && type != NEW_SESSION_ESTABLISHED) ||
            (type == NEW_SESSION_ESTABLISHED && (elem).first != sessionIndex) ||
            !(elem).second->isEstablished())
        {
            continue;
        }
        if ((_BGPSessions[sessionIndex]->getType() == IGP && (elem).second->getType() == EGP) ||
            _BGPSessions[sessionIndex]->getType() == EGP ||
            type == ROUTE_DESTINATION_CHANGED ||
            type == NEW_SESSION_ESTABLISHED)
        {
            BgpUpdateNlri NLRI;
            BgpUpdatePathAttributeList content;

            unsigned int nbAS = entry->getASCount();
            content.setAsPathArraySize(1);
            content.getMutableAsPath(0).setValueArraySize(1);
            content.getMutableAsPath(0).getMutableValue(0).setType(AS_SEQUENCE);
            //RFC 4271 : set My AS in first position if it is not already
            if (entry->getAS(0) != _myAS) {
                content.getMutableAsPath(0).getMutableValue(0).setAsValueArraySize(nbAS + 1);
                content.getMutableAsPath(0).getMutableValue(0).setLength(1);
                content.getMutableAsPath(0).getMutableValue(0).setAsValue(0, _myAS);
                for (unsigned int j = 1; j < nbAS + 1; j++) {
                    content.getMutableAsPath(0).getMutableValue(0).setAsValue(j, entry->getAS(j - 1));
                }
            }
            else {
                content.getMutableAsPath(0).getMutableValue(0).setAsValueArraySize(nbAS);
                content.getMutableAsPath(0).getMutableValue(0).setLength(1);
                for (unsigned int j = 0; j < nbAS; j++) {
                    content.getMutableAsPath(0).getMutableValue(0).setAsValue(j, entry->getAS(j));
                }
            }

            InterfaceEntry *iftEntry = (elem).second->getLinkIntf();
            content.getMutableOrigin().setValue((elem).second->getType());
            content.getMutableNextHop().setValue(iftEntry->ipv4Data()->getIPAddress());
            Ipv4Address netMask = entry->getNetmask();
            NLRI.prefix = entry->getDestination().doAnd(netMask);
            NLRI.length = (unsigned char)netMask.getNetmaskLength();
            {
                Packet *pk = new Packet("BGPUpdate");
                const auto& updateMsg = makeShared<BgpUpdateMessage>();
                updateMsg->setPathAttributeListArraySize(1);
                updateMsg->setPathAttributeList(content);
                updateMsg->setNLRI(NLRI);
                updateMsg->markImmutable();
                pk->pushHeader(updateMsg);
                (elem).second->getSocket()->send(pk);
                (elem).second->addUpdateMsgSent();
            }
        }
    }
}

bool BgpRouting::checkExternalRoute(const Ipv4Route *route)
{
    Ipv4Address OSPFRoute;
    OSPFRoute = route->getDestination();
    ospf::OspfRouting *ospf = getModuleFromPar<ospf::OspfRouting>(par("ospfRoutingModule"), this);
    bool returnValue = ospf->checkExternalRoute(OSPFRoute);
    return returnValue;
}

void BgpRouting::loadTimerConfig(cXMLElementList& timerConfig, simtime_t *delayTab)
{
    for (auto & elem : timerConfig) {
        std::string nodeName = (elem)->getTagName();
        if (nodeName == "connectRetryTime") {
            delayTab[0] = (double)atoi((elem)->getNodeValue());
        }
        else if (nodeName == "holdTime") {
            delayTab[1] = (double)atoi((elem)->getNodeValue());
        }
        else if (nodeName == "keepAliveTime") {
            delayTab[2] = (double)atoi((elem)->getNodeValue());
        }
        else if (nodeName == "startDelay") {
            delayTab[3] = (double)atoi((elem)->getNodeValue());
        }
    }
}

AsId BgpRouting::findMyAS(cXMLElementList& asList, int& outRouterPosition)
{
    // find my own IPv4 address in the configuration file and return the AS id under which it is configured
    // and also the 1 based position of the entry inside the AS config element
    for (auto & elem : asList) {
        cXMLElementList routerList = (elem)->getChildrenByTagName("Router");
        outRouterPosition = 1;
        for (auto & routerList_routerListIt : routerList) {
            Ipv4Address routerAddr = Ipv4Address((routerList_routerListIt)->getAttribute("interAddr"));
            for (int i = 0; i < _inft->getNumInterfaces(); i++) {
                if (_inft->getInterface(i)->ipv4Data()->getIPAddress() == routerAddr)
                    return atoi((routerList_routerListIt)->getParentNode()->getAttribute("id"));
            }
            outRouterPosition++;
        }
    }

    return 0;
}

void BgpRouting::loadSessionConfig(cXMLElementList& sessionList, simtime_t *delayTab)
{
    simtime_t saveStartDelay = delayTab[3];
    for (auto sessionListIt = sessionList.begin(); sessionListIt != sessionList.end(); sessionListIt++, delayTab[3] = saveStartDelay) {
        const char *exterAddr = (*sessionListIt)->getFirstChild()->getAttribute("exterAddr");
        Ipv4Address routerAddr1 = Ipv4Address(exterAddr);
        exterAddr = (*sessionListIt)->getLastChild()->getAttribute("exterAddr");
        Ipv4Address routerAddr2 = Ipv4Address(exterAddr);
        if (isInInterfaceTable(_inft, routerAddr1) == -1 && isInInterfaceTable(_inft, routerAddr2) == -1) {
            continue;
        }
        Ipv4Address peerAddr;
        if (isInInterfaceTable(_inft, routerAddr1) != -1) {
            peerAddr = routerAddr2;
            delayTab[3] += atoi((*sessionListIt)->getAttribute("id"));
        }
        else {
            peerAddr = routerAddr1;
            delayTab[3] += atoi((*sessionListIt)->getAttribute("id")) + 0.5;
        }
        if (peerAddr.isUnspecified()) {
            throw cRuntimeError("BGP Error: No valid external address for session ID : %s", (*sessionListIt)->getAttribute("id"));
        }

        SessionId newSessionID = createSession(EGP, peerAddr.str().c_str());
        _BGPSessions[newSessionID]->setTimers(delayTab);
        TcpSocket *socketListenEGP = new TcpSocket();
        _BGPSessions[newSessionID]->setSocketListen(socketListenEGP);
    }
}

std::vector<const char *> BgpRouting::loadASConfig(cXMLElementList& ASConfig)
{
    //create deny Lists
    std::vector<const char *> routerInSameASList;

    for (auto & elem : ASConfig) {
        std::string nodeName = (elem)->getTagName();
        if (nodeName == "Router") {
            if (isInInterfaceTable(_inft, Ipv4Address((elem)->getAttribute("interAddr"))) == -1) {
                routerInSameASList.push_back((elem)->getAttribute("interAddr"));
            }
            continue;
        }
        if (nodeName == "DenyRoute" || nodeName == "DenyRouteIN" || nodeName == "DenyRouteOUT") {
            RoutingTableEntry *entry = new RoutingTableEntry();     //FIXME Who will delete this entry?
            entry->setDestination(Ipv4Address((elem)->getAttribute("Address")));
            entry->setNetmask(Ipv4Address((elem)->getAttribute("Netmask")));
            if (nodeName == "DenyRouteIN") {
                _prefixListIN.push_back(entry);
                _prefixListINOUT.push_back(entry);
            }
            else if (nodeName == "DenyRouteOUT") {
                _prefixListOUT.push_back(entry);
                _prefixListINOUT.push_back(entry);
            }
            else {
                _prefixListIN.push_back(entry);
                _prefixListOUT.push_back(entry);
                _prefixListINOUT.push_back(entry);
            }
        }
        else if (nodeName == "DenyAS" || nodeName == "DenyASIN" || nodeName == "DenyASOUT") {
            AsId ASCur = atoi((elem)->getNodeValue());
            if (nodeName == "DenyASIN") {
                _ASListIN.push_back(ASCur);
            }
            else if (nodeName == "DenyASOUT") {
                _ASListOUT.push_back(ASCur);
            }
            else {
                _ASListIN.push_back(ASCur);
                _ASListOUT.push_back(ASCur);
            }
        }
        else {
            throw cRuntimeError("BGP Error: unknown element named '%s' for AS %u", nodeName.c_str(), _myAS);
        }
    }
    return routerInSameASList;
}

void BgpRouting::loadConfigFromXML(cXMLElement *bgpConfig)
{
    if (strcmp(bgpConfig->getTagName(), "BGPConfig"))
        throw cRuntimeError("Cannot read BGP configuration, unaccepted '%s' node at %s", bgpConfig->getTagName(), bgpConfig->getSourceLocation());

    // load bgp timer parameters informations
    simtime_t delayTab[NB_TIMERS];
    cXMLElement *paramNode = bgpConfig->getElementByPath("TimerParams");
    if (paramNode == nullptr)
        throw cRuntimeError("BGP Error: No configuration for BGP timer parameters");

    cXMLElementList timerConfig = paramNode->getChildren();
    loadTimerConfig(timerConfig, delayTab);

    //find my AS
    cXMLElementList ASList = bgpConfig->getElementsByTagName("AS");
    int routerPosition;
    _myAS = findMyAS(ASList, routerPosition);
    if (_myAS == 0)
        throw cRuntimeError("BGP Error:  No AS configuration for Router ID: %s", _rt->getRouterId().str().c_str());

    // load EGP Session informations
    cXMLElementList sessionList = bgpConfig->getElementsByTagName("Session");
    simtime_t saveStartDelay = delayTab[3];
    loadSessionConfig(sessionList, delayTab);
    delayTab[3] = saveStartDelay;

    // load AS information
    char ASXPath[32];
    sprintf(ASXPath, "AS[@id='%d']", _myAS);

    cXMLElement *ASNode = bgpConfig->getElementByPath(ASXPath);
    std::vector<const char *> routerInSameASList;
    if (ASNode == nullptr)
        throw cRuntimeError("BGP Error:  No configuration for AS ID: %d", _myAS);

    cXMLElementList ASConfig = ASNode->getChildren();
    routerInSameASList = loadASConfig(ASConfig);

    //create IGP Session(s)
    if (routerInSameASList.size()) {
        unsigned int routerPeerPosition = 1;
        delayTab[3] += sessionList.size() * 2;
        for (auto it = routerInSameASList.begin(); it != routerInSameASList.end(); it++, routerPeerPosition++) {
            SessionId newSessionID;
            TcpSocket *socketListenIGP = new TcpSocket();
            newSessionID = createSession(IGP, (*it));
            delayTab[3] += calculateStartDelay(routerInSameASList.size(), routerPosition, routerPeerPosition);
            _BGPSessions[newSessionID]->setTimers(delayTab);
            _BGPSessions[newSessionID]->setSocketListen(socketListenIGP);
        }
    }
}

unsigned int BgpRouting::calculateStartDelay(int rtListSize, unsigned char rtPosition, unsigned char rtPeerPosition)
{
    unsigned int startDelay = 0;
    if (rtPeerPosition == 1) {
        if (rtPosition == 1) {
            startDelay = 1;
        }
        else {
            startDelay = (rtPosition - 1) * 2;
        }
        return startDelay;
    }

    if (rtPosition < rtPeerPosition) {
        startDelay = 2;
    }
    else if (rtPosition > rtPeerPosition) {
        startDelay = (rtListSize - 1) * 2 - 2 * (rtPeerPosition - 2);
    }
    else {
        startDelay = (rtListSize - 1) * 2 + 1;
    }
    return startDelay;
}

SessionId BgpRouting::createSession(BgpSessionType typeSession, const char *peerAddr)
{
    BgpSession *newSession = new BgpSession(*this);
    SessionId newSessionId;
    SessionInfo info;

    info.sessionType = typeSession;
    info.ASValue = _myAS;
    info.routerID = _rt->getRouterId();
    info.peerAddr.set(peerAddr);
    if (typeSession == EGP) {
        info.linkIntf = _rt->getInterfaceForDestAddr(info.peerAddr);
        if (info.linkIntf == nullptr) {
            throw cRuntimeError("BGP Error: No configuration interface for peer address: %s", peerAddr);
        }
        info.sessionID = info.peerAddr.getInt() + info.linkIntf->ipv4Data()->getIPAddress().getInt();
    }
    else {
        info.sessionID = info.peerAddr.getInt() + info.routerID.getInt();
    }
    newSessionId = info.sessionID;
    newSession->setInfo(info);
    _BGPSessions[newSessionId] = newSession;

    return newSessionId;
}

SessionId BgpRouting::findIdFromPeerAddr(std::map<SessionId, BgpSession *> sessions, Ipv4Address peerAddr)
{
    for (auto & session : sessions)
    {
        if ((session).second->getPeerAddr().equals(peerAddr)) {
            return (session).first;
        }
    }
    return -1;
}

/*
 *  Delete BGP Routing entry, if the route deleted correctly return true, false else.
 *  Side effects when returns true:
 *      _BGPRoutingTable changed, iterators on _BGPRoutingTable will be invalid.
 */
bool BgpRouting::deleteBGPRoutingEntry(RoutingTableEntry *entry)
{
    for (auto it = _BGPRoutingTable.begin();
         it != _BGPRoutingTable.end(); it++)
    {
        if (((*it)->getDestination().getInt() & (*it)->getNetmask().getInt()) ==
            (entry->getDestination().getInt() & entry->getNetmask().getInt()))
        {
            _BGPRoutingTable.erase(it);
            _rt->deleteRoute(entry);
            return true;
        }
    }
    return false;
}

/*return index of the IPv4 table if the route is found, -1 else*/
int BgpRouting::isInRoutingTable(IIpv4RoutingTable *rtTable, Ipv4Address addr)
{
    for (int i = 0; i < rtTable->getNumRoutes(); i++) {
        const Ipv4Route *entry = rtTable->getRoute(i);
        if (Ipv4Address::maskedAddrAreEqual(addr, entry->getDestination(), entry->getNetmask())) {
            return i;
        }
    }
    return -1;
}

int BgpRouting::isInInterfaceTable(IInterfaceTable *ifTable, Ipv4Address addr)
{
    for (int i = 0; i < ifTable->getNumInterfaces(); i++) {
        if (ifTable->getInterface(i)->ipv4Data()->getIPAddress() == addr) {
            return i;
        }
    }
    return -1;
}

SessionId BgpRouting::findIdFromSocketConnId(std::map<SessionId, BgpSession *> sessions, int connId)
{
    for (auto & session : sessions)
    {
        TcpSocket *socket = (session).second->getSocket();
        if (socket->getConnectionId() == connId) {
            return (session).first;
        }
    }
    return -1;
}

/*return index of the table if the route is found, -1 else*/
unsigned long BgpRouting::isInTable(std::vector<RoutingTableEntry *> rtTable, RoutingTableEntry *entry)
{
    for (unsigned long i = 0; i < rtTable.size(); i++) {
        RoutingTableEntry *entryCur = rtTable[i];
        if ((entry->getDestination().getInt() & entry->getNetmask().getInt()) ==
            (entryCur->getDestination().getInt() & entryCur->getNetmask().getInt()))
        {
            return i;
        }
    }
    return -1;
}

/*return true if the AS is found, false else*/
bool BgpRouting::isInASList(std::vector<AsId> ASList, RoutingTableEntry *entry)
{
    for (auto & elem : ASList) {
        for (unsigned int i = 0; i < entry->getASCount(); i++) {
            if ((elem) == entry->getAS(i)) {
                return true;
            }
        }
    }
    return false;
}

/*return true if OSPF exists, false else*/
bool BgpRouting::ospfExist(IIpv4RoutingTable *rtTable)
{
    for (int i = 0; i < rtTable->getNumRoutes(); i++) {
        if (rtTable->getRoute(i)->getSourceType() == IRoute::OSPF) {
            return true;
        }
    }
    return false;
}

unsigned char BgpRouting::asLoopDetection(RoutingTableEntry *entry, AsId myAS)
{
    for (unsigned int i = 1; i < entry->getASCount(); i++) {
        if (myAS == entry->getAS(i)) {
            return ASLOOP_DETECTED;
        }
    }
    return ASLOOP_NO_DETECTED;
}

/*return sessionID if the session is found, -1 else*/
SessionId BgpRouting::findNextSession(BgpSessionType type, bool startSession)
{
    SessionId sessionID = -1;
    for (auto & elem : _BGPSessions)
    {
        if ((elem).second->getType() == type && !(elem).second->isEstablished()) {
            sessionID = (elem).first;
            break;
        }
    }
    if (startSession == true && type == IGP && sessionID != (SessionId)-1) {
        InterfaceEntry *linkIntf = _rt->getInterfaceForDestAddr(_BGPSessions[sessionID]->getPeerAddr());
        if (linkIntf == nullptr) {
            throw cRuntimeError("No configuration interface for peer address: %s", _BGPSessions[sessionID]->getPeerAddr().str().c_str());
        }
        _BGPSessions[sessionID]->setlinkIntf(linkIntf);
        _BGPSessions[sessionID]->startConnection();
    }
    return sessionID;
}

} // namespace bgp

} // namespace inet

