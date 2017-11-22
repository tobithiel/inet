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

#include "inet/applications/tcpapp/TcpSrvHostApp.h"

#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/NodeOperations.h"
#include "inet/common/INETUtils.h"

namespace inet {

Define_Module(TcpSrvHostApp);

void TcpSrvHostApp::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_APPLICATION_LAYER) {
        nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
        if (isNodeUp())
            start();
    }
}

void TcpSrvHostApp::start()
{
    const char *localAddress = par("localAddress");
    int localPort = par("localPort");

    serverSocket.setOutputGate(gate("socketOut"));
    serverSocket.bind(localAddress[0] ? L3Address(localAddress) : L3Address(), localPort);
    serverSocket.listen();
}

void TcpSrvHostApp::stop()
{
    //FIXME close sockets?

    // remove and delete threads
    while (!threadSet.empty())
        removeThread(*threadSet.begin());
}

void TcpSrvHostApp::crash()
{
    // remove and delete threads
    while (!threadSet.empty())
        removeThread(*threadSet.begin());
}

void TcpSrvHostApp::refreshDisplay() const
{
    char buf[32];
    sprintf(buf, "%d threads", socketMap.size());
    getDisplayString().setTagArg("t", 0, buf);
}

void TcpSrvHostApp::handleMessage(cMessage *msg)
{
    if (!isNodeUp()) {
        //TODO error?
        EV_ERROR << "message " << msg->getFullName() << "(" << msg->getClassName() << ") arrived when module is down\n";
        delete msg;
    }
    else if (msg->isSelfMessage()) {
        TcpServerThreadBase *thread = (TcpServerThreadBase *)msg->getContextPointer();
        if (threadSet.find(thread) == threadSet.end())
            throw cRuntimeError("Invalid thread pointer in the timer (msg->contextPointer is invalid)");
        thread->timerExpired(msg);
    }
    else {
        TcpSocket *socket = socketMap.findSocketFor(msg);

        if (!socket) {
            // new connection -- create new socket object and server process
            socket = new TcpSocket(msg);
            socket->setOutputGate(gate("socketOut"));

            const char *serverThreadModuleType = par("serverThreadModuleType");
            cModuleType *moduleType = cModuleType::get(serverThreadModuleType);
            char name[80];
            sprintf(name, "thread_%i", socket->getConnectionId());
            TcpServerThreadBase *proc = check_and_cast<TcpServerThreadBase *>(moduleType->create(name, this));
            proc->finalizeParameters();

            proc->callInitialize();

            socket->setCallbackObject(proc);
            proc->init(this, socket);

            socketMap.addSocket(socket);
            threadSet.insert(proc);
        }

        socket->processMessage(msg);
    }
}

void TcpSrvHostApp::finish()
{
    stop();
}

void TcpSrvHostApp::removeThread(TcpServerThreadBase *thread)
{
    // remove socket
    socketMap.removeSocket(thread->getSocket());
    threadSet.erase(thread);

    // remove thread object
    delete thread;
}

bool TcpSrvHostApp::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    Enter_Method_Silent();
    if (dynamic_cast<NodeStartOperation *>(operation)) {
        if ((NodeStartOperation::Stage)stage == NodeStartOperation::STAGE_APPLICATION_LAYER) {
            start();
        }
    }
    else if (dynamic_cast<NodeShutdownOperation *>(operation)) {
        if ((NodeShutdownOperation::Stage)stage == NodeShutdownOperation::STAGE_APPLICATION_LAYER) {
            stop();
        }
    }
    else if (dynamic_cast<NodeCrashOperation *>(operation)) {
        if ((NodeCrashOperation::Stage)stage == NodeCrashOperation::STAGE_CRASH)
            crash();
    }
    else
        throw cRuntimeError("Unsupported lifecycle operation '%s'", operation->getClassName());
    return true;
}

void TcpServerThreadBase::refreshDisplay() const
{
    getDisplayString().setTagArg("t", 0, TcpSocket::stateName(sock->getState()));
}


} // namespace inet

