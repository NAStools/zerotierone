/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2016  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "../version.h"

#include "Constants.hpp"
#include "Node.hpp"
#include "RuntimeEnvironment.hpp"
#include "NetworkController.hpp"
#include "Switch.hpp"
#include "Multicaster.hpp"
#include "Topology.hpp"
#include "Buffer.hpp"
#include "Packet.hpp"
#include "Address.hpp"
#include "Identity.hpp"
#include "SelfAwareness.hpp"
#include "Cluster.hpp"
#include "DeferredPackets.hpp"

const struct sockaddr_storage ZT_SOCKADDR_NULL = {0};

namespace ZeroTier {

/****************************************************************************/
/* Public Node interface (C++, exposed via CAPI bindings)                   */
/****************************************************************************/

Node::Node(
	uint64_t now,
	void *uptr,
	ZT_DataStoreGetFunction dataStoreGetFunction,
	ZT_DataStorePutFunction dataStorePutFunction,
	ZT_WirePacketSendFunction wirePacketSendFunction,
	ZT_VirtualNetworkFrameFunction virtualNetworkFrameFunction,
	ZT_VirtualNetworkConfigFunction virtualNetworkConfigFunction,
	ZT_PathCheckFunction pathCheckFunction,
	ZT_EventCallback eventCallback) :
	_RR(this),
	RR(&_RR),
	_uPtr(uptr),
	_dataStoreGetFunction(dataStoreGetFunction),
	_dataStorePutFunction(dataStorePutFunction),
	_wirePacketSendFunction(wirePacketSendFunction),
	_virtualNetworkFrameFunction(virtualNetworkFrameFunction),
	_virtualNetworkConfigFunction(virtualNetworkConfigFunction),
	_pathCheckFunction(pathCheckFunction),
	_eventCallback(eventCallback),
	_networks(),
	_networks_m(),
	_prngStreamPtr(0),
	_now(now),
	_lastPingCheck(0),
	_lastHousekeepingRun(0)
{
	_online = false;

	// Use Salsa20 alone as a high-quality non-crypto PRNG
	{
		char foo[32];
		Utils::getSecureRandom(foo,32);
		_prng.init(foo,256,foo);
		memset(_prngStream,0,sizeof(_prngStream));
		_prng.encrypt12(_prngStream,_prngStream,sizeof(_prngStream));
	}

	{
		std::string idtmp(dataStoreGet("identity.secret"));
		if ((!idtmp.length())||(!RR->identity.fromString(idtmp))||(!RR->identity.hasPrivate())) {
			TRACE("identity.secret not found, generating...");
			RR->identity.generate();
			idtmp = RR->identity.toString(true);
			if (!dataStorePut("identity.secret",idtmp,true))
				throw std::runtime_error("unable to write identity.secret");
		}
		RR->publicIdentityStr = RR->identity.toString(false);
		RR->secretIdentityStr = RR->identity.toString(true);
		idtmp = dataStoreGet("identity.public");
		if (idtmp != RR->publicIdentityStr) {
			if (!dataStorePut("identity.public",RR->publicIdentityStr,false))
				throw std::runtime_error("unable to write identity.public");
		}
	}

	try {
		RR->sw = new Switch(RR);
		RR->mc = new Multicaster(RR);
		RR->topology = new Topology(RR);
		RR->sa = new SelfAwareness(RR);
		RR->dp = new DeferredPackets(RR);
	} catch ( ... ) {
		delete RR->dp;
		delete RR->sa;
		delete RR->topology;
		delete RR->mc;
		delete RR->sw;
		throw;
	}

	postEvent(ZT_EVENT_UP);
}

Node::~Node()
{
	Mutex::Lock _l(_networks_m);

	_networks.clear(); // ensure that networks are destroyed before shutdow

	RR->dpEnabled = 0;
	delete RR->dp;
	delete RR->sa;
	delete RR->topology;
	delete RR->mc;
	delete RR->sw;
#ifdef ZT_ENABLE_CLUSTER
	delete RR->cluster;
#endif
}

ZT_ResultCode Node::processWirePacket(
	uint64_t now,
	const struct sockaddr_storage *localAddress,
	const struct sockaddr_storage *remoteAddress,
	const void *packetData,
	unsigned int packetLength,
	volatile uint64_t *nextBackgroundTaskDeadline)
{
	_now = now;
	RR->sw->onRemotePacket(*(reinterpret_cast<const InetAddress *>(localAddress)),*(reinterpret_cast<const InetAddress *>(remoteAddress)),packetData,packetLength);
	return ZT_RESULT_OK;
}

ZT_ResultCode Node::processVirtualNetworkFrame(
	uint64_t now,
	uint64_t nwid,
	uint64_t sourceMac,
	uint64_t destMac,
	unsigned int etherType,
	unsigned int vlanId,
	const void *frameData,
	unsigned int frameLength,
	volatile uint64_t *nextBackgroundTaskDeadline)
{
	_now = now;
	SharedPtr<Network> nw(this->network(nwid));
	if (nw) {
		RR->sw->onLocalEthernet(nw,MAC(sourceMac),MAC(destMac),etherType,vlanId,frameData,frameLength);
		return ZT_RESULT_OK;
	} else return ZT_RESULT_ERROR_NETWORK_NOT_FOUND;
}

class _PingPeersThatNeedPing
{
public:
	_PingPeersThatNeedPing(const RuntimeEnvironment *renv,uint64_t now,const std::vector<NetworkConfig::Relay> &relays) :
		lastReceiveFromUpstream(0),
		RR(renv),
		_now(now),
		_relays(relays),
		_world(RR->topology->world())
	{
	}

	uint64_t lastReceiveFromUpstream; // tracks last time we got a packet from an 'upstream' peer like a root or a relay

	inline void operator()(Topology &t,const SharedPtr<Peer> &p)
	{
		bool upstream = false;
		InetAddress stableEndpoint4,stableEndpoint6;

		// If this is a world root, pick (if possible) both an IPv4 and an IPv6 stable endpoint to use if link isn't currently alive.
		for(std::vector<World::Root>::const_iterator r(_world.roots().begin());r!=_world.roots().end();++r) {
			if (r->identity == p->identity()) {
				upstream = true;
				for(unsigned long k=0,ptr=(unsigned long)RR->node->prng();k<(unsigned long)r->stableEndpoints.size();++k) {
					const InetAddress &addr = r->stableEndpoints[ptr++ % r->stableEndpoints.size()];
					if (!stableEndpoint4) {
						if (addr.ss_family == AF_INET)
							stableEndpoint4 = addr;
					}
					if (!stableEndpoint6) {
						if (addr.ss_family == AF_INET6)
							stableEndpoint6 = addr;
					}
				}
				break;
			}
		}

		if (!upstream) {
			// If I am a root server, only ping other root servers -- roots don't ping "down"
			// since that would just be a waste of bandwidth and could potentially cause route
			// flapping in Cluster mode.
			if (RR->topology->amRoot())
				return;

			// Check for network preferred relays, also considered 'upstream' and thus always
			// pinged to keep links up. If they have stable addresses we will try them there.
			for(std::vector<NetworkConfig::Relay>::const_iterator r(_relays.begin());r!=_relays.end();++r) {
				if (r->address == p->address()) {
					stableEndpoint4 = r->phy4;
					stableEndpoint6 = r->phy6;
					upstream = true;
					break;
				}
			}
		}

		if (upstream) {
			// "Upstream" devices are roots and relays and get special treatment -- they stay alive
			// forever and we try to keep (if available) both IPv4 and IPv6 channels open to them.
			bool needToContactIndirect = true;
			if (p->doPingAndKeepalive(_now,AF_INET)) {
				needToContactIndirect = false;
			} else {
				if (stableEndpoint4) {
					needToContactIndirect = false;
					p->sendHELLO(InetAddress(),stableEndpoint4,_now);
				}
			}
			if (p->doPingAndKeepalive(_now,AF_INET6)) {
				needToContactIndirect = false;
			} else {
				if (stableEndpoint6) {
					needToContactIndirect = false;
					p->sendHELLO(InetAddress(),stableEndpoint6,_now);
				}
			}

			if (needToContactIndirect) {
				// If this is an upstream and we have no stable endpoint for either IPv4 or IPv6,
				// send a NOP indirectly if possible to see if we can get to this peer in any
				// way whatsoever. This will e.g. find network preferred relays that lack
				// stable endpoints by using root servers.
				Packet outp(p->address(),RR->identity.address(),Packet::VERB_NOP);
				RR->sw->send(outp,true,0);
			}

			lastReceiveFromUpstream = std::max(p->lastReceive(),lastReceiveFromUpstream);
		} else if (p->activelyTransferringFrames(_now)) {
			// Normal nodes get their preferred link kept alive if the node has generated frame traffic recently
			p->doPingAndKeepalive(_now,0);
		}
	}

private:
	const RuntimeEnvironment *RR;
	uint64_t _now;
	const std::vector<NetworkConfig::Relay> &_relays;
	World _world;
};

ZT_ResultCode Node::processBackgroundTasks(uint64_t now,volatile uint64_t *nextBackgroundTaskDeadline)
{
	_now = now;
	Mutex::Lock bl(_backgroundTasksLock);

	unsigned long timeUntilNextPingCheck = ZT_PING_CHECK_INVERVAL;
	const uint64_t timeSinceLastPingCheck = now - _lastPingCheck;
	if (timeSinceLastPingCheck >= ZT_PING_CHECK_INVERVAL) {
		try {
			_lastPingCheck = now;

			// Get relays and networks that need config without leaving the mutex locked
			std::vector< NetworkConfig::Relay > networkRelays;
			std::vector< SharedPtr<Network> > needConfig;
			{
				Mutex::Lock _l(_networks_m);
				for(std::vector< std::pair< uint64_t,SharedPtr<Network> > >::const_iterator n(_networks.begin());n!=_networks.end();++n) {
					if (((now - n->second->lastConfigUpdate()) >= ZT_NETWORK_AUTOCONF_DELAY)||(!n->second->hasConfig())) {
						needConfig.push_back(n->second);
					}
					if (n->second->hasConfig()) {
						std::vector<NetworkConfig::Relay> r(n->second->config().relays());
						networkRelays.insert(networkRelays.end(),r.begin(),r.end());
					}
				}
			}

			// Request updated configuration for networks that need it
			for(std::vector< SharedPtr<Network> >::const_iterator n(needConfig.begin());n!=needConfig.end();++n)
				(*n)->requestConfiguration();

			// Do pings and keepalives
			_PingPeersThatNeedPing pfunc(RR,now,networkRelays);
			RR->topology->eachPeer<_PingPeersThatNeedPing &>(pfunc);

			// Update online status, post status change as event
			const bool oldOnline = _online;
			_online = (((now - pfunc.lastReceiveFromUpstream) < ZT_PEER_ACTIVITY_TIMEOUT)||(RR->topology->amRoot()));
			if (oldOnline != _online)
				postEvent(_online ? ZT_EVENT_ONLINE : ZT_EVENT_OFFLINE);
		} catch ( ... ) {
			return ZT_RESULT_FATAL_ERROR_INTERNAL;
		}
	} else {
		timeUntilNextPingCheck -= (unsigned long)timeSinceLastPingCheck;
	}

	if ((now - _lastHousekeepingRun) >= ZT_HOUSEKEEPING_PERIOD) {
		try {
			_lastHousekeepingRun = now;
			RR->topology->clean(now);
			RR->sa->clean(now);
			RR->mc->clean(now);
		} catch ( ... ) {
			return ZT_RESULT_FATAL_ERROR_INTERNAL;
		}
	}

	try {
#ifdef ZT_ENABLE_CLUSTER
		// If clustering is enabled we have to call cluster->doPeriodicTasks() very often, so we override normal timer deadline behavior
		if (RR->cluster) {
			RR->sw->doTimerTasks(now);
			RR->cluster->doPeriodicTasks();
			*nextBackgroundTaskDeadline = now + ZT_CLUSTER_PERIODIC_TASK_PERIOD; // this is really short so just tick at this rate
		} else {
#endif
			*nextBackgroundTaskDeadline = now + (uint64_t)std::max(std::min(timeUntilNextPingCheck,RR->sw->doTimerTasks(now)),(unsigned long)ZT_CORE_TIMER_TASK_GRANULARITY);
#ifdef ZT_ENABLE_CLUSTER
		}
#endif
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}

	return ZT_RESULT_OK;
}

ZT_ResultCode Node::join(uint64_t nwid,void *uptr)
{
	Mutex::Lock _l(_networks_m);
	SharedPtr<Network> nw = _network(nwid);
	if(!nw)
		_networks.push_back(std::pair< uint64_t,SharedPtr<Network> >(nwid,SharedPtr<Network>(new Network(RR,nwid,uptr))));
	std::sort(_networks.begin(),_networks.end()); // will sort by nwid since it's the first in a pair<>
	return ZT_RESULT_OK;
}

ZT_ResultCode Node::leave(uint64_t nwid,void **uptr)
{
	std::vector< std::pair< uint64_t,SharedPtr<Network> > > newn;
	Mutex::Lock _l(_networks_m);
	for(std::vector< std::pair< uint64_t,SharedPtr<Network> > >::const_iterator n(_networks.begin());n!=_networks.end();++n) {
		if (n->first != nwid)
			newn.push_back(*n);
		else {
			if (uptr)
				*uptr = n->second->userPtr();
			n->second->destroy();
		}
	}
	_networks.swap(newn);
	return ZT_RESULT_OK;
}

ZT_ResultCode Node::multicastSubscribe(uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	SharedPtr<Network> nw(this->network(nwid));
	if (nw) {
		nw->multicastSubscribe(MulticastGroup(MAC(multicastGroup),(uint32_t)(multicastAdi & 0xffffffff)));
		return ZT_RESULT_OK;
	} else return ZT_RESULT_ERROR_NETWORK_NOT_FOUND;
}

ZT_ResultCode Node::multicastUnsubscribe(uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	SharedPtr<Network> nw(this->network(nwid));
	if (nw) {
		nw->multicastUnsubscribe(MulticastGroup(MAC(multicastGroup),(uint32_t)(multicastAdi & 0xffffffff)));
		return ZT_RESULT_OK;
	} else return ZT_RESULT_ERROR_NETWORK_NOT_FOUND;
}

uint64_t Node::address() const
{
	return RR->identity.address().toInt();
}

void Node::status(ZT_NodeStatus *status) const
{
	status->address = RR->identity.address().toInt();
	status->worldId = RR->topology->worldId();
	status->worldTimestamp = RR->topology->worldTimestamp();
	status->publicIdentity = RR->publicIdentityStr.c_str();
	status->secretIdentity = RR->secretIdentityStr.c_str();
	status->online = _online ? 1 : 0;
}

ZT_PeerList *Node::peers() const
{
	std::vector< std::pair< Address,SharedPtr<Peer> > > peers(RR->topology->allPeers());
	std::sort(peers.begin(),peers.end());

	char *buf = (char *)::malloc(sizeof(ZT_PeerList) + (sizeof(ZT_Peer) * peers.size()));
	if (!buf)
		return (ZT_PeerList *)0;
	ZT_PeerList *pl = (ZT_PeerList *)buf;
	pl->peers = (ZT_Peer *)(buf + sizeof(ZT_PeerList));

	pl->peerCount = 0;
	for(std::vector< std::pair< Address,SharedPtr<Peer> > >::iterator pi(peers.begin());pi!=peers.end();++pi) {
		ZT_Peer *p = &(pl->peers[pl->peerCount++]);
		p->address = pi->second->address().toInt();
		p->lastUnicastFrame = pi->second->lastUnicastFrame();
		p->lastMulticastFrame = pi->second->lastMulticastFrame();
		if (pi->second->remoteVersionKnown()) {
			p->versionMajor = pi->second->remoteVersionMajor();
			p->versionMinor = pi->second->remoteVersionMinor();
			p->versionRev = pi->second->remoteVersionRevision();
		} else {
			p->versionMajor = -1;
			p->versionMinor = -1;
			p->versionRev = -1;
		}
		p->latency = pi->second->latency();
		p->role = RR->topology->isRoot(pi->second->identity()) ? ZT_PEER_ROLE_ROOT : ZT_PEER_ROLE_LEAF;

		std::vector<Path> paths(pi->second->paths());
		Path *bestPath = pi->second->getBestPath(_now);
		p->pathCount = 0;
		for(std::vector<Path>::iterator path(paths.begin());path!=paths.end();++path) {
			memcpy(&(p->paths[p->pathCount].address),&(path->address()),sizeof(struct sockaddr_storage));
			p->paths[p->pathCount].lastSend = path->lastSend();
			p->paths[p->pathCount].lastReceive = path->lastReceived();
			p->paths[p->pathCount].active = path->active(_now) ? 1 : 0;
			p->paths[p->pathCount].preferred = ((bestPath)&&(*path == *bestPath)) ? 1 : 0;
			p->paths[p->pathCount].trustedPathId = RR->topology->getOutboundPathTrust(path->address());
			++p->pathCount;
		}
	}

	return pl;
}

ZT_VirtualNetworkConfig *Node::networkConfig(uint64_t nwid) const
{
	Mutex::Lock _l(_networks_m);
	SharedPtr<Network> nw = _network(nwid);
	if(nw) {
		ZT_VirtualNetworkConfig *nc = (ZT_VirtualNetworkConfig *)::malloc(sizeof(ZT_VirtualNetworkConfig));
		nw->externalConfig(nc);
		return nc;
	}
	return (ZT_VirtualNetworkConfig *)0;
}

ZT_VirtualNetworkList *Node::networks() const
{
	Mutex::Lock _l(_networks_m);

	char *buf = (char *)::malloc(sizeof(ZT_VirtualNetworkList) + (sizeof(ZT_VirtualNetworkConfig) * _networks.size()));
	if (!buf)
		return (ZT_VirtualNetworkList *)0;
	ZT_VirtualNetworkList *nl = (ZT_VirtualNetworkList *)buf;
	nl->networks = (ZT_VirtualNetworkConfig *)(buf + sizeof(ZT_VirtualNetworkList));

	nl->networkCount = 0;
	for(std::vector< std::pair< uint64_t,SharedPtr<Network> > >::const_iterator n(_networks.begin());n!=_networks.end();++n)
		n->second->externalConfig(&(nl->networks[nl->networkCount++]));

	return nl;
}

void Node::freeQueryResult(void *qr)
{
	if (qr)
		::free(qr);
}

int Node::addLocalInterfaceAddress(const struct sockaddr_storage *addr)
{
	if (Path::isAddressValidForPath(*(reinterpret_cast<const InetAddress *>(addr)))) {
		Mutex::Lock _l(_directPaths_m);
		if (std::find(_directPaths.begin(),_directPaths.end(),*(reinterpret_cast<const InetAddress *>(addr))) == _directPaths.end()) {
			_directPaths.push_back(*(reinterpret_cast<const InetAddress *>(addr)));
			return 1;
		}
	}
	return 0;
}

void Node::clearLocalInterfaceAddresses()
{
	Mutex::Lock _l(_directPaths_m);
	_directPaths.clear();
}

void Node::setNetconfMaster(void *networkControllerInstance)
{
	RR->localNetworkController = reinterpret_cast<NetworkController *>(networkControllerInstance);
}

ZT_ResultCode Node::circuitTestBegin(ZT_CircuitTest *test,void (*reportCallback)(ZT_Node *,ZT_CircuitTest *,const ZT_CircuitTestReport *))
{
	if (test->hopCount > 0) {
		try {
			Packet outp(Address(),RR->identity.address(),Packet::VERB_CIRCUIT_TEST);
			RR->identity.address().appendTo(outp);
			outp.append((uint16_t)((test->reportAtEveryHop != 0) ? 0x03 : 0x02));
			outp.append((uint64_t)test->timestamp);
			outp.append((uint64_t)test->testId);
			outp.append((uint16_t)0); // originator credential length, updated later
			if (test->credentialNetworkId) {
				outp.append((uint8_t)0x01);
				outp.append((uint64_t)test->credentialNetworkId);
				outp.setAt<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 23,(uint16_t)9);
			}
			outp.append((uint16_t)0);
			C25519::Signature sig(RR->identity.sign(reinterpret_cast<const char *>(outp.data()) + ZT_PACKET_IDX_PAYLOAD,outp.size() - ZT_PACKET_IDX_PAYLOAD));
			outp.append((uint16_t)sig.size());
			outp.append(sig.data,(unsigned int)sig.size());
			outp.append((uint16_t)0); // originator doesn't need an extra credential, since it's the originator
			for(unsigned int h=1;h<test->hopCount;++h) {
				outp.append((uint8_t)0);
				outp.append((uint8_t)(test->hops[h].breadth & 0xff));
				for(unsigned int a=0;a<test->hops[h].breadth;++a)
					Address(test->hops[h].addresses[a]).appendTo(outp);
			}

			for(unsigned int a=0;a<test->hops[0].breadth;++a) {
				outp.newInitializationVector();
				outp.setDestination(Address(test->hops[0].addresses[a]));
				RR->sw->send(outp,true,0);
			}
		} catch ( ... ) {
			return ZT_RESULT_FATAL_ERROR_INTERNAL; // probably indicates FIFO too big for packet
		}
	}

	{
		test->_internalPtr = reinterpret_cast<void *>(reportCallback);
		Mutex::Lock _l(_circuitTests_m);
		if (std::find(_circuitTests.begin(),_circuitTests.end(),test) == _circuitTests.end())
			_circuitTests.push_back(test);
	}

	return ZT_RESULT_OK;
}

void Node::circuitTestEnd(ZT_CircuitTest *test)
{
	Mutex::Lock _l(_circuitTests_m);
	for(;;) {
		std::vector< ZT_CircuitTest * >::iterator ct(std::find(_circuitTests.begin(),_circuitTests.end(),test));
		if (ct == _circuitTests.end())
			break;
		else _circuitTests.erase(ct);
	}
}

ZT_ResultCode Node::clusterInit(
	unsigned int myId,
	const struct sockaddr_storage *zeroTierPhysicalEndpoints,
	unsigned int numZeroTierPhysicalEndpoints,
	int x,
	int y,
	int z,
	void (*sendFunction)(void *,unsigned int,const void *,unsigned int),
	void *sendFunctionArg,
	int (*addressToLocationFunction)(void *,const struct sockaddr_storage *,int *,int *,int *),
	void *addressToLocationFunctionArg)
{
#ifdef ZT_ENABLE_CLUSTER
	if (RR->cluster)
		return ZT_RESULT_ERROR_BAD_PARAMETER;

	std::vector<InetAddress> eps;
	for(unsigned int i=0;i<numZeroTierPhysicalEndpoints;++i)
		eps.push_back(InetAddress(zeroTierPhysicalEndpoints[i]));
	std::sort(eps.begin(),eps.end());
	RR->cluster = new Cluster(RR,myId,eps,x,y,z,sendFunction,sendFunctionArg,addressToLocationFunction,addressToLocationFunctionArg);

	return ZT_RESULT_OK;
#else
	return ZT_RESULT_ERROR_UNSUPPORTED_OPERATION;
#endif
}

ZT_ResultCode Node::clusterAddMember(unsigned int memberId)
{
#ifdef ZT_ENABLE_CLUSTER
	if (!RR->cluster)
		return ZT_RESULT_ERROR_BAD_PARAMETER;
	RR->cluster->addMember((uint16_t)memberId);
	return ZT_RESULT_OK;
#else
	return ZT_RESULT_ERROR_UNSUPPORTED_OPERATION;
#endif
}

void Node::clusterRemoveMember(unsigned int memberId)
{
#ifdef ZT_ENABLE_CLUSTER
	if (RR->cluster)
		RR->cluster->removeMember((uint16_t)memberId);
#endif
}

void Node::clusterHandleIncomingMessage(const void *msg,unsigned int len)
{
#ifdef ZT_ENABLE_CLUSTER
	if (RR->cluster)
		RR->cluster->handleIncomingStateMessage(msg,len);
#endif
}

void Node::clusterStatus(ZT_ClusterStatus *cs)
{
	if (!cs)
		return;
#ifdef ZT_ENABLE_CLUSTER
	if (RR->cluster)
		RR->cluster->status(*cs);
	else
#endif
	memset(cs,0,sizeof(ZT_ClusterStatus));
}

void Node::backgroundThreadMain()
{
	++RR->dpEnabled;
	for(;;) {
		try {
			if (RR->dp->process() < 0)
				break;
		} catch ( ... ) {} // sanity check -- should not throw
	}
	--RR->dpEnabled;
}

/****************************************************************************/
/* Node methods used only within node/                                      */
/****************************************************************************/

std::string Node::dataStoreGet(const char *name)
{
	char buf[1024];
	std::string r;
	unsigned long olen = 0;
	do {
		long n = _dataStoreGetFunction(reinterpret_cast<ZT_Node *>(this),_uPtr,name,buf,sizeof(buf),(unsigned long)r.length(),&olen);
		if (n <= 0)
			return std::string();
		r.append(buf,n);
	} while (r.length() < olen);
	return r;
}

bool Node::shouldUsePathForZeroTierTraffic(const InetAddress &localAddress,const InetAddress &remoteAddress)
{
	if (!Path::isAddressValidForPath(remoteAddress))
		return false;

	{
		Mutex::Lock _l(_networks_m);
		for(std::vector< std::pair< uint64_t, SharedPtr<Network> > >::const_iterator i=_networks.begin();i!=_networks.end();++i) {
			if (i->second->hasConfig()) {
				for(unsigned int k=0;k<i->second->config().staticIpCount;++k) {
					if (i->second->config().staticIps[k].containsAddress(remoteAddress))
						return false;
				}
			}
		}
	}

	if (_pathCheckFunction)
		return (_pathCheckFunction(reinterpret_cast<ZT_Node *>(this),_uPtr,reinterpret_cast<const struct sockaddr_storage *>(&localAddress),reinterpret_cast<const struct sockaddr_storage *>(&remoteAddress)) != 0);
	else return true;
}

#ifdef ZT_TRACE
void Node::postTrace(const char *module,unsigned int line,const char *fmt,...)
{
	static Mutex traceLock;

	va_list ap;
	char tmp1[1024],tmp2[1024],tmp3[256];

	Mutex::Lock _l(traceLock);

	time_t now = (time_t)(_now / 1000ULL);
#ifdef __WINDOWS__
	ctime_s(tmp3,sizeof(tmp3),&now);
	char *nowstr = tmp3;
#else
	char *nowstr = ctime_r(&now,tmp3);
#endif
	unsigned long nowstrlen = (unsigned long)strlen(nowstr);
	if (nowstr[nowstrlen-1] == '\n')
		nowstr[--nowstrlen] = (char)0;
	if (nowstr[nowstrlen-1] == '\r')
		nowstr[--nowstrlen] = (char)0;

	va_start(ap,fmt);
	vsnprintf(tmp2,sizeof(tmp2),fmt,ap);
	va_end(ap);
	tmp2[sizeof(tmp2)-1] = (char)0;

	Utils::snprintf(tmp1,sizeof(tmp1),"[%s] %s:%u %s",nowstr,module,line,tmp2);
	postEvent(ZT_EVENT_TRACE,tmp1);
}
#endif // ZT_TRACE

uint64_t Node::prng()
{
	unsigned int p = (++_prngStreamPtr % (sizeof(_prngStream) / sizeof(uint64_t)));
	if (!p)
		_prng.encrypt12(_prngStream,_prngStream,sizeof(_prngStream));
	return _prngStream[p];
}

void Node::postCircuitTestReport(const ZT_CircuitTestReport *report)
{
	std::vector< ZT_CircuitTest * > toNotify;
	{
		Mutex::Lock _l(_circuitTests_m);
		for(std::vector< ZT_CircuitTest * >::iterator i(_circuitTests.begin());i!=_circuitTests.end();++i) {
			if ((*i)->testId == report->testId)
				toNotify.push_back(*i);
		}
	}
	for(std::vector< ZT_CircuitTest * >::iterator i(toNotify.begin());i!=toNotify.end();++i)
		(reinterpret_cast<void (*)(ZT_Node *,ZT_CircuitTest *,const ZT_CircuitTestReport *)>((*i)->_internalPtr))(reinterpret_cast<ZT_Node *>(this),*i,report);
}

void Node::setTrustedPaths(const struct sockaddr_storage *networks,const uint64_t *ids,unsigned int count)
{
	RR->topology->setTrustedPaths(reinterpret_cast<const InetAddress *>(networks),ids,count);
}

} // namespace ZeroTier

/****************************************************************************/
/* CAPI bindings                                                            */
/****************************************************************************/

extern "C" {

enum ZT_ResultCode ZT_Node_new(
	ZT_Node **node,
	void *uptr,
	uint64_t now,
	ZT_DataStoreGetFunction dataStoreGetFunction,
	ZT_DataStorePutFunction dataStorePutFunction,
	ZT_WirePacketSendFunction wirePacketSendFunction,
	ZT_VirtualNetworkFrameFunction virtualNetworkFrameFunction,
	ZT_VirtualNetworkConfigFunction virtualNetworkConfigFunction,
	ZT_PathCheckFunction pathCheckFunction,
	ZT_EventCallback eventCallback)
{
	*node = (ZT_Node *)0;
	try {
		*node = reinterpret_cast<ZT_Node *>(new ZeroTier::Node(now,uptr,dataStoreGetFunction,dataStorePutFunction,wirePacketSendFunction,virtualNetworkFrameFunction,virtualNetworkConfigFunction,pathCheckFunction,eventCallback));
		return ZT_RESULT_OK;
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch (std::runtime_error &exc) {
		return ZT_RESULT_FATAL_ERROR_DATA_STORE_FAILED;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

void ZT_Node_delete(ZT_Node *node)
{
	try {
		delete (reinterpret_cast<ZeroTier::Node *>(node));
	} catch ( ... ) {}
}

enum ZT_ResultCode ZT_Node_processWirePacket(
	ZT_Node *node,
	uint64_t now,
	const struct sockaddr_storage *localAddress,
	const struct sockaddr_storage *remoteAddress,
	const void *packetData,
	unsigned int packetLength,
	volatile uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processWirePacket(now,localAddress,remoteAddress,packetData,packetLength,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_OK; // "OK" since invalid packets are simply dropped, but the system is still up
	}
}

enum ZT_ResultCode ZT_Node_processVirtualNetworkFrame(
	ZT_Node *node,
	uint64_t now,
	uint64_t nwid,
	uint64_t sourceMac,
	uint64_t destMac,
	unsigned int etherType,
	unsigned int vlanId,
	const void *frameData,
	unsigned int frameLength,
	volatile uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processVirtualNetworkFrame(now,nwid,sourceMac,destMac,etherType,vlanId,frameData,frameLength,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_processBackgroundTasks(ZT_Node *node,uint64_t now,volatile uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processBackgroundTasks(now,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_join(ZT_Node *node,uint64_t nwid,void *uptr)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->join(nwid,uptr);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_leave(ZT_Node *node,uint64_t nwid,void **uptr)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->leave(nwid,uptr);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_multicastSubscribe(ZT_Node *node,uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->multicastSubscribe(nwid,multicastGroup,multicastAdi);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_multicastUnsubscribe(ZT_Node *node,uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->multicastUnsubscribe(nwid,multicastGroup,multicastAdi);
	} catch (std::bad_alloc &exc) {
		return ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

uint64_t ZT_Node_address(ZT_Node *node)
{
	return reinterpret_cast<ZeroTier::Node *>(node)->address();
}

void ZT_Node_status(ZT_Node *node,ZT_NodeStatus *status)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->status(status);
	} catch ( ... ) {}
}

ZT_PeerList *ZT_Node_peers(ZT_Node *node)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->peers();
	} catch ( ... ) {
		return (ZT_PeerList *)0;
	}
}

ZT_VirtualNetworkConfig *ZT_Node_networkConfig(ZT_Node *node,uint64_t nwid)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->networkConfig(nwid);
	} catch ( ... ) {
		return (ZT_VirtualNetworkConfig *)0;
	}
}

ZT_VirtualNetworkList *ZT_Node_networks(ZT_Node *node)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->networks();
	} catch ( ... ) {
		return (ZT_VirtualNetworkList *)0;
	}
}

void ZT_Node_freeQueryResult(ZT_Node *node,void *qr)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->freeQueryResult(qr);
	} catch ( ... ) {}
}

int ZT_Node_addLocalInterfaceAddress(ZT_Node *node,const struct sockaddr_storage *addr)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->addLocalInterfaceAddress(addr);
	} catch ( ... ) {
		return 0;
	}
}

void ZT_Node_clearLocalInterfaceAddresses(ZT_Node *node)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->clearLocalInterfaceAddresses();
	} catch ( ... ) {}
}

void ZT_Node_setNetconfMaster(ZT_Node *node,void *networkControllerInstance)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->setNetconfMaster(networkControllerInstance);
	} catch ( ... ) {}
}

enum ZT_ResultCode ZT_Node_circuitTestBegin(ZT_Node *node,ZT_CircuitTest *test,void (*reportCallback)(ZT_Node *,ZT_CircuitTest *,const ZT_CircuitTestReport *))
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->circuitTestBegin(test,reportCallback);
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

void ZT_Node_circuitTestEnd(ZT_Node *node,ZT_CircuitTest *test)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->circuitTestEnd(test);
	} catch ( ... ) {}
}

enum ZT_ResultCode ZT_Node_clusterInit(
	ZT_Node *node,
	unsigned int myId,
	const struct sockaddr_storage *zeroTierPhysicalEndpoints,
	unsigned int numZeroTierPhysicalEndpoints,
	int x,
	int y,
	int z,
	void (*sendFunction)(void *,unsigned int,const void *,unsigned int),
	void *sendFunctionArg,
	int (*addressToLocationFunction)(void *,const struct sockaddr_storage *,int *,int *,int *),
	void *addressToLocationFunctionArg)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->clusterInit(myId,zeroTierPhysicalEndpoints,numZeroTierPhysicalEndpoints,x,y,z,sendFunction,sendFunctionArg,addressToLocationFunction,addressToLocationFunctionArg);
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT_ResultCode ZT_Node_clusterAddMember(ZT_Node *node,unsigned int memberId)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->clusterAddMember(memberId);
	} catch ( ... ) {
		return ZT_RESULT_FATAL_ERROR_INTERNAL;
	}
}

void ZT_Node_clusterRemoveMember(ZT_Node *node,unsigned int memberId)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->clusterRemoveMember(memberId);
	} catch ( ... ) {}
}

void ZT_Node_clusterHandleIncomingMessage(ZT_Node *node,const void *msg,unsigned int len)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->clusterHandleIncomingMessage(msg,len);
	} catch ( ... ) {}
}

void ZT_Node_clusterStatus(ZT_Node *node,ZT_ClusterStatus *cs)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->clusterStatus(cs);
	} catch ( ... ) {}
}

void ZT_Node_setTrustedPaths(ZT_Node *node,const struct sockaddr_storage *networks,const uint64_t *ids,unsigned int count)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->setTrustedPaths(networks,ids,count);
	} catch ( ... ) {}
}

void ZT_Node_backgroundThreadMain(ZT_Node *node)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->backgroundThreadMain();
	} catch ( ... ) {}
}

void ZT_version(int *major,int *minor,int *revision)
{
	if (major) *major = ZEROTIER_ONE_VERSION_MAJOR;
	if (minor) *minor = ZEROTIER_ONE_VERSION_MINOR;
	if (revision) *revision = ZEROTIER_ONE_VERSION_REVISION;
}

} // extern "C"
