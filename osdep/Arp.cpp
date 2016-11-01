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
#include <string.h>
#include <stdlib.h>

#include "Arp.hpp"
#include "OSUtils.hpp"

namespace ZeroTier {

static const uint8_t ARP_REQUEST_HEADER[8] = { 0x00,0x01,0x08,0x00,0x06,0x04,0x00,0x01 };
static const uint8_t ARP_RESPONSE_HEADER[8] = { 0x00,0x01,0x08,0x00,0x06,0x04,0x00,0x02 };

Arp::Arp() :
	_cache(256),
	_lastCleaned(OSUtils::now())
{
}

void Arp::addLocal(uint32_t ip,const MAC &mac)
{
	_ArpEntry &e = _cache[ip];
	e.lastQuerySent = 0; // local IP
	e.lastResponseReceived = 0; // local IP
	e.mac = mac;
	e.local = true;
}

void Arp::remove(uint32_t ip)
{
	_cache.erase(ip);
}

uint32_t Arp::processIncomingArp(const void *arp,unsigned int len,void *response,unsigned int &responseLen,MAC &responseDest)
{
	const uint64_t now = OSUtils::now();
	uint32_t ip = 0;

	responseLen = 0;
	responseDest.zero();

	if (len >= 28) {
		if (!memcmp(arp,ARP_REQUEST_HEADER,8)) {
			// Respond to ARP requests for locally-known IPs
			_ArpEntry *targetEntry = _cache.get(reinterpret_cast<const uint32_t *>(arp)[6]);
			if ((targetEntry)&&(targetEntry->local)) {
				memcpy(response,ARP_RESPONSE_HEADER,8);
				targetEntry->mac.copyTo(reinterpret_cast<uint8_t *>(response) + 8,6);
				memcpy(reinterpret_cast<uint8_t *>(response) + 14,reinterpret_cast<const uint8_t *>(arp) + 24,4);
				memcpy(reinterpret_cast<uint8_t *>(response) + 18,reinterpret_cast<const uint8_t *>(arp) + 8,10);
				responseLen = 28;
				responseDest.setTo(reinterpret_cast<const uint8_t *>(arp) + 8,6);
			}
		} else if (!memcmp(arp,ARP_RESPONSE_HEADER,8)) {
			// Learn cache entries for remote IPs from relevant ARP replies
			uint32_t responseIp = 0;
			memcpy(&responseIp,reinterpret_cast<const uint8_t *>(arp) + 14,4);
			_ArpEntry *queryEntry = _cache.get(responseIp);
			if ((queryEntry)&&(!queryEntry->local)&&((now - queryEntry->lastQuerySent) <= ZT_ARP_QUERY_MAX_TTL)) {
				queryEntry->lastResponseReceived = now;
				queryEntry->mac.setTo(reinterpret_cast<const uint8_t *>(arp) + 8,6);
				ip = responseIp;
			}
		}
	}

	if ((now - _lastCleaned) >= ZT_ARP_EXPIRE) {
		_lastCleaned = now;
		Hashtable< uint32_t,_ArpEntry >::Iterator i(_cache);
		uint32_t *k = (uint32_t *)0;
		_ArpEntry *v = (_ArpEntry *)0;
		while (i.next(k,v)) {
			if ((!v->local)&&((now - v->lastResponseReceived) >= ZT_ARP_EXPIRE))
				_cache.erase(*k);
		}
	}

	return ip;
}

MAC Arp::query(const MAC &localMac,uint32_t localIp,uint32_t targetIp,void *query,unsigned int &queryLen,MAC &queryDest)
{
	const uint64_t now = OSUtils::now();

	_ArpEntry &e = _cache[targetIp];

	if ( ((e.mac)&&((now - e.lastResponseReceived) >= (ZT_ARP_EXPIRE / 3))) ||
	     ((!e.mac)&&((now - e.lastQuerySent) >= ZT_ARP_QUERY_INTERVAL)) ) {
		e.lastQuerySent = now;

		uint8_t *q = reinterpret_cast<uint8_t *>(query);
		memcpy(q,ARP_REQUEST_HEADER,8); q += 8; // ARP request header information, always the same
		localMac.copyTo(q,6); q += 6; // sending host MAC address
        memcpy(q,&localIp,4); q += 4; // sending host IP (IP already in big-endian byte order)
		memset(q,0,6); q += 6; // sending zeros for target MAC address as thats what we want to find
		memcpy(q,&targetIp,4); // target IP address for resolution (IP already in big-endian byte order)
		queryLen = 28;
		if (e.mac)
			queryDest = e.mac; // confirmation query, send directly to address holder
		else queryDest = (uint64_t)0xffffffffffffULL; // broadcast query
	} else {
		queryLen = 0;
		queryDest.zero();
	}

	return e.mac;
}

} // namespace ZeroTier
