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

#include <algorithm>

#include "Constants.hpp"
#include "RuntimeEnvironment.hpp"
#include "SharedPtr.hpp"
#include "Multicaster.hpp"
#include "Topology.hpp"
#include "Switch.hpp"
#include "Packet.hpp"
#include "Peer.hpp"
#include "C25519.hpp"
#include "CertificateOfMembership.hpp"
#include "Node.hpp"

namespace ZeroTier {

Multicaster::Multicaster(const RuntimeEnvironment *renv) :
	RR(renv),
	_groups(1024),
	_groups_m()
{
}

Multicaster::~Multicaster()
{
}

void Multicaster::addMultiple(uint64_t now,uint64_t nwid,const MulticastGroup &mg,const void *addresses,unsigned int count,unsigned int totalKnown)
{
	const unsigned char *p = (const unsigned char *)addresses;
	const unsigned char *e = p + (5 * count);
	Mutex::Lock _l(_groups_m);
	MulticastGroupStatus &gs = _groups[Multicaster::Key(nwid,mg)];
	while (p != e) {
		_add(now,nwid,mg,gs,Address(p,5));
		p += 5;
	}
}

void Multicaster::remove(uint64_t nwid,const MulticastGroup &mg,const Address &member)
{
	Mutex::Lock _l(_groups_m);
	MulticastGroupStatus *s = _groups.get(Multicaster::Key(nwid,mg));
	if (s) {
		for(std::vector<MulticastGroupMember>::iterator m(s->members.begin());m!=s->members.end();++m) {
			if (m->address == member) {
				s->members.erase(m);
				break;
			}
		}
	}
}

unsigned int Multicaster::gather(const Address &queryingPeer,uint64_t nwid,const MulticastGroup &mg,Buffer<ZT_PROTO_MAX_PACKET_LENGTH> &appendTo,unsigned int limit) const
{
	unsigned char *p;
	unsigned int added = 0,i,k,rptr,totalKnown = 0;
	uint64_t a,picked[(ZT_PROTO_MAX_PACKET_LENGTH / 5) + 2];

	if (!limit)
		return 0;
	else if (limit > 0xffff)
		limit = 0xffff;

	const unsigned int totalAt = appendTo.size();
	appendTo.addSize(4); // sizeof(uint32_t)
	const unsigned int addedAt = appendTo.size();
	appendTo.addSize(2); // sizeof(uint16_t)

	{ // Return myself if I am a member of this group
		SharedPtr<Network> network(RR->node->network(nwid));
		if ((network)&&(network->subscribedToMulticastGroup(mg,true))) {
			RR->identity.address().appendTo(appendTo);
			++totalKnown;
			++added;
		}
	}

	Mutex::Lock _l(_groups_m);

	const MulticastGroupStatus *s = _groups.get(Multicaster::Key(nwid,mg));
	if ((s)&&(!s->members.empty())) {
		totalKnown += (unsigned int)s->members.size();

		// Members are returned in random order so that repeated gather queries
		// will return different subsets of a large multicast group.
		k = 0;
		while ((added < limit)&&(k < s->members.size())&&((appendTo.size() + ZT_ADDRESS_LENGTH) <= ZT_UDP_DEFAULT_PAYLOAD_MTU)) {
			rptr = (unsigned int)RR->node->prng();

restart_member_scan:
			a = s->members[rptr % (unsigned int)s->members.size()].address.toInt();
			for(i=0;i<k;++i) {
				if (picked[i] == a) {
					++rptr;
					goto restart_member_scan;
				}
			}
			picked[k++] = a;

			if (queryingPeer.toInt() != a) { // do not return the peer that is making the request as a result
				p = (unsigned char *)appendTo.appendField(ZT_ADDRESS_LENGTH);
				*(p++) = (unsigned char)((a >> 32) & 0xff);
				*(p++) = (unsigned char)((a >> 24) & 0xff);
				*(p++) = (unsigned char)((a >> 16) & 0xff);
				*(p++) = (unsigned char)((a >> 8) & 0xff);
				*p = (unsigned char)(a & 0xff);
				++added;
			}
		}
	}

	appendTo.setAt(totalAt,(uint32_t)totalKnown);
	appendTo.setAt(addedAt,(uint16_t)added);

	//TRACE("..MC Multicaster::gather() attached %u of %u peers for %.16llx/%s (2)",n,(unsigned int)(gs->second.members.size() - skipped),nwid,mg.toString().c_str());

	return added;
}

std::vector<Address> Multicaster::getMembers(uint64_t nwid,const MulticastGroup &mg,unsigned int limit) const
{
	std::vector<Address> ls;
	Mutex::Lock _l(_groups_m);
	const MulticastGroupStatus *s = _groups.get(Multicaster::Key(nwid,mg));
	if (!s)
		return ls;
	for(std::vector<MulticastGroupMember>::const_reverse_iterator m(s->members.rbegin());m!=s->members.rend();++m) {
		ls.push_back(m->address);
		if (ls.size() >= limit)
			break;
	}
	return ls;
}

void Multicaster::send(
	const CertificateOfMembership *com,
	unsigned int limit,
	uint64_t now,
	uint64_t nwid,
	const std::vector<Address> &alwaysSendTo,
	const MulticastGroup &mg,
	const MAC &src,
	unsigned int etherType,
	const void *data,
	unsigned int len)
{
	unsigned long idxbuf[8194];
	unsigned long *indexes = idxbuf;

	try {
		Mutex::Lock _l(_groups_m);
		MulticastGroupStatus &gs = _groups[Multicaster::Key(nwid,mg)];

		if (!gs.members.empty()) {
			// Allocate a memory buffer if group is monstrous
			if (gs.members.size() > (sizeof(idxbuf) / sizeof(unsigned long)))
				indexes = new unsigned long[gs.members.size()];

			// Generate a random permutation of member indexes
			for(unsigned long i=0;i<gs.members.size();++i)
				indexes[i] = i;
			for(unsigned long i=(unsigned long)gs.members.size()-1;i>0;--i) {
				unsigned long j = (unsigned long)RR->node->prng() % (i + 1);
				unsigned long tmp = indexes[j];
				indexes[j] = indexes[i];
				indexes[i] = tmp;
			}
		}

		if (gs.members.size() >= limit) {
			// Skip queue if we already have enough members to complete the send operation
			OutboundMulticast out;

			out.init(
				RR,
				now,
				nwid,
				com,
				limit,
				1, // we'll still gather a little from peers to keep multicast list fresh
				src,
				mg,
				etherType,
				data,
				len);

			unsigned int count = 0;

			for(std::vector<Address>::const_iterator ast(alwaysSendTo.begin());ast!=alwaysSendTo.end();++ast) {
				if (*ast != RR->identity.address()) {
					out.sendOnly(RR,*ast); // optimization: don't use dedup log if it's a one-pass send
					if (++count >= limit)
						break;
				}
			}

			unsigned long idx = 0;
			while ((count < limit)&&(idx < gs.members.size())) {
				Address ma(gs.members[indexes[idx++]].address);
				if (std::find(alwaysSendTo.begin(),alwaysSendTo.end(),ma) == alwaysSendTo.end()) {
					out.sendOnly(RR,ma); // optimization: don't use dedup log if it's a one-pass send
					++count;
				}
			}
		} else {
			unsigned int gatherLimit = (limit - (unsigned int)gs.members.size()) + 1;

			if ((gs.members.empty())||((now - gs.lastExplicitGather) >= ZT_MULTICAST_EXPLICIT_GATHER_DELAY)) {
				gs.lastExplicitGather = now;
				SharedPtr<Peer> explicitGatherPeers[2];
				explicitGatherPeers[0] = RR->topology->getBestRoot();
				const Address nwidc(Network::controllerFor(nwid));
				if (nwidc != RR->identity.address())
					explicitGatherPeers[1] = RR->topology->getPeer(nwidc);
				for(unsigned int k=0;k<2;++k) {
					const SharedPtr<Peer> &p = explicitGatherPeers[k];
					if (!p)
						continue;
					//TRACE(">>MC upstream GATHER up to %u for group %.16llx/%s",gatherLimit,nwid,mg.toString().c_str());

					const CertificateOfMembership *com = (CertificateOfMembership *)0;
					{
						SharedPtr<Network> nw(RR->node->network(nwid));
						if ((nw)&&(nw->hasConfig())&&(nw->config().com)&&(nw->config().isPrivate())&&(p->needsOurNetworkMembershipCertificate(nwid,now,true)))
							com = &(nw->config().com);
					}

					Packet outp(p->address(),RR->identity.address(),Packet::VERB_MULTICAST_GATHER);
					outp.append(nwid);
					outp.append((uint8_t)(com ? 0x01 : 0x00));
					mg.mac().appendTo(outp);
					outp.append((uint32_t)mg.adi());
					outp.append((uint32_t)gatherLimit);
					if (com)
						com->serialize(outp);
					RR->sw->send(outp,true,0);
				}
				gatherLimit = 0;
			}

			gs.txQueue.push_back(OutboundMulticast());
			OutboundMulticast &out = gs.txQueue.back();

			out.init(
				RR,
				now,
				nwid,
				com,
				limit,
				gatherLimit,
				src,
				mg,
				etherType,
				data,
				len);

			unsigned int count = 0;

			for(std::vector<Address>::const_iterator ast(alwaysSendTo.begin());ast!=alwaysSendTo.end();++ast) {
				if (*ast != RR->identity.address()) {
					out.sendAndLog(RR,*ast);
					if (++count >= limit)
						break;
				}
			}

			unsigned long idx = 0;
			while ((count < limit)&&(idx < gs.members.size())) {
				Address ma(gs.members[indexes[idx++]].address);
				if (std::find(alwaysSendTo.begin(),alwaysSendTo.end(),ma) == alwaysSendTo.end()) {
					out.sendAndLog(RR,ma);
					++count;
				}
			}
		}
	} catch ( ... ) {} // this is a sanity check to catch any failures and make sure indexes[] still gets deleted

	// Free allocated memory buffer if any
	if (indexes != idxbuf)
		delete [] indexes;
}

void Multicaster::clean(uint64_t now)
{
	Mutex::Lock _l(_groups_m);

	Multicaster::Key *k = (Multicaster::Key *)0;
	MulticastGroupStatus *s = (MulticastGroupStatus *)0;
	Hashtable<Multicaster::Key,MulticastGroupStatus>::Iterator mm(_groups);
	while (mm.next(k,s)) {
		for(std::list<OutboundMulticast>::iterator tx(s->txQueue.begin());tx!=s->txQueue.end();) {
			if ((tx->expired(now))||(tx->atLimit()))
				s->txQueue.erase(tx++);
			else ++tx;
		}

		unsigned long count = 0;
		{
			std::vector<MulticastGroupMember>::iterator reader(s->members.begin());
			std::vector<MulticastGroupMember>::iterator writer(reader);
			while (reader != s->members.end()) {
				if ((now - reader->timestamp) < ZT_MULTICAST_LIKE_EXPIRE) {
					*writer = *reader;
					++writer;
					++count;
				}
				++reader;
			}
		}

		if (count) {
			s->members.resize(count);
		} else if (s->txQueue.empty()) {
			_groups.erase(*k);
		} else {
			s->members.clear();
		}
	}
}

void Multicaster::_add(uint64_t now,uint64_t nwid,const MulticastGroup &mg,MulticastGroupStatus &gs,const Address &member)
{
	// assumes _groups_m is locked

	// Do not add self -- even if someone else returns it
	if (member == RR->identity.address())
		return;

	for(std::vector<MulticastGroupMember>::iterator m(gs.members.begin());m!=gs.members.end();++m) {
		if (m->address == member) {
			m->timestamp = now;
			return;
		}
	}

	gs.members.push_back(MulticastGroupMember(member,now));

	//TRACE("..MC %s joined multicast group %.16llx/%s via %s",member.toString().c_str(),nwid,mg.toString().c_str(),((learnedFrom) ? learnedFrom.toString().c_str() : "(direct)"));

	for(std::list<OutboundMulticast>::iterator tx(gs.txQueue.begin());tx!=gs.txQueue.end();) {
		if (tx->atLimit())
			gs.txQueue.erase(tx++);
		else {
			tx->sendIfNew(RR,member);
			if (tx->atLimit())
				gs.txQueue.erase(tx++);
			else ++tx;
		}
	}
}

} // namespace ZeroTier
