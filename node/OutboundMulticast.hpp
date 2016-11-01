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

#ifndef ZT_OUTBOUNDMULTICAST_HPP
#define ZT_OUTBOUNDMULTICAST_HPP

#include <stdint.h>

#include <vector>
#include <algorithm>

#include "Constants.hpp"
#include "MAC.hpp"
#include "MulticastGroup.hpp"
#include "Address.hpp"
#include "Packet.hpp"

namespace ZeroTier {

class CertificateOfMembership;
class RuntimeEnvironment;

/**
 * An outbound multicast packet
 *
 * This object isn't guarded by a mutex; caller must synchronize access.
 */
class OutboundMulticast
{
public:
	/**
	 * Create an uninitialized outbound multicast
	 *
	 * It must be initialized with init().
	 */
	OutboundMulticast() {}

	/**
	 * Initialize outbound multicast
	 *
	 * @param RR Runtime environment
	 * @param timestamp Creation time
	 * @param nwid Network ID
	 * @param com Certificate of membership or NULL if none available
	 * @param limit Multicast limit for desired number of packets to send
	 * @param gatherLimit Number to lazily/implicitly gather with this frame or 0 for none
	 * @param src Source MAC address of frame or NULL to imply compute from sender ZT address
	 * @param dest Destination multicast group (MAC + ADI)
	 * @param etherType 16-bit Ethernet type ID
	 * @param payload Data
	 * @param len Length of data
	 * @throws std::out_of_range Data too large to fit in a MULTICAST_FRAME
	 */
	void init(
		const RuntimeEnvironment *RR,
		uint64_t timestamp,
		uint64_t nwid,
		const CertificateOfMembership *com,
		unsigned int limit,
		unsigned int gatherLimit,
		const MAC &src,
		const MulticastGroup &dest,
		unsigned int etherType,
		const void *payload,
		unsigned int len);

	/**
	 * @return Multicast creation time
	 */
	inline uint64_t timestamp() const throw() { return _timestamp; }

	/**
	 * @param now Current time
	 * @return True if this multicast is expired (has exceeded transmit timeout)
	 */
	inline bool expired(uint64_t now) const throw() { return ((now - _timestamp) >= ZT_MULTICAST_TRANSMIT_TIMEOUT); }

	/**
	 * @return True if this outbound multicast has been sent to enough peers
	 */
	inline bool atLimit() const throw() { return (_alreadySentTo.size() >= _limit); }

	/**
	 * Just send without checking log
	 *
	 * @param RR Runtime environment
	 * @param toAddr Destination address
	 */
	void sendOnly(const RuntimeEnvironment *RR,const Address &toAddr);

	/**
	 * Just send and log but do not check sent log
	 *
	 * @param RR Runtime environment
	 * @param toAddr Destination address
	 */
	inline void sendAndLog(const RuntimeEnvironment *RR,const Address &toAddr)
	{
		_alreadySentTo.push_back(toAddr);
		sendOnly(RR,toAddr);
	}

	/**
	 * Try to send this to a given peer if it hasn't been sent to them already
	 *
	 * @param RR Runtime environment
	 * @param toAddr Destination address
	 * @return True if address is new and packet was sent to switch, false if duplicate
	 */
	inline bool sendIfNew(const RuntimeEnvironment *RR,const Address &toAddr)
	{
		if (std::find(_alreadySentTo.begin(),_alreadySentTo.end(),toAddr) == _alreadySentTo.end()) {
			sendAndLog(RR,toAddr);
			return true;
		} else return false;
	}

private:
	uint64_t _timestamp;
	uint64_t _nwid;
	unsigned int _limit;
	Packet _packetNoCom;
	Packet _packetWithCom;
	std::vector<Address> _alreadySentTo;
	bool _haveCom;
};

} // namespace ZeroTier

#endif
