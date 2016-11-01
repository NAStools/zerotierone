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

#ifndef ZT_ADDRESS_HPP
#define ZT_ADDRESS_HPP

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "Constants.hpp"
#include "Utils.hpp"
#include "Buffer.hpp"

namespace ZeroTier {

/**
 * A ZeroTier address
 */
class Address
{
public:
	Address()
		throw() :
		_a(0)
	{
	}

	Address(const Address &a)
		throw() :
		_a(a._a)
	{
	}

	Address(uint64_t a)
		throw() :
		_a(a & 0xffffffffffULL)
	{
	}

	Address(const char *s)
		throw()
	{
		unsigned char foo[ZT_ADDRESS_LENGTH];
		setTo(foo,Utils::unhex(s,foo,ZT_ADDRESS_LENGTH));
	}

	Address(const std::string &s)
		throw()
	{
		unsigned char foo[ZT_ADDRESS_LENGTH];
		setTo(foo,Utils::unhex(s.c_str(),foo,ZT_ADDRESS_LENGTH));
	}

	/**
	 * @param bits Raw address -- 5 bytes, big-endian byte order
	 * @param len Length of array
	 */
	Address(const void *bits,unsigned int len)
		throw()
	{
		setTo(bits,len);
	}

	inline Address &operator=(const Address &a)
		throw()
	{
		_a = a._a;
		return *this;
	}

	inline Address &operator=(const uint64_t a)
		throw()
	{
		_a = (a & 0xffffffffffULL);
		return *this;
	}

	/**
	 * @param bits Raw address -- 5 bytes, big-endian byte order
	 * @param len Length of array
	 */
	inline void setTo(const void *bits,unsigned int len)
		throw()
	{
		if (len < ZT_ADDRESS_LENGTH) {
			_a = 0;
			return;
		}
		const unsigned char *b = (const unsigned char *)bits;
		uint64_t a = ((uint64_t)*b++) << 32;
		a |= ((uint64_t)*b++) << 24;
		a |= ((uint64_t)*b++) << 16;
		a |= ((uint64_t)*b++) << 8;
		a |= ((uint64_t)*b);
		_a = a;
	}

	/**
	 * @param bits Buffer to hold 5-byte address in big-endian byte order
	 * @param len Length of array
	 */
	inline void copyTo(void *bits,unsigned int len) const
		throw()
	{
		if (len < ZT_ADDRESS_LENGTH)
			return;
		unsigned char *b = (unsigned char *)bits;
		*(b++) = (unsigned char)((_a >> 32) & 0xff);
		*(b++) = (unsigned char)((_a >> 24) & 0xff);
		*(b++) = (unsigned char)((_a >> 16) & 0xff);
		*(b++) = (unsigned char)((_a >> 8) & 0xff);
		*b = (unsigned char)(_a & 0xff);
	}

	/**
	 * Append to a buffer in big-endian byte order
	 *
	 * @param b Buffer to append to
	 */
	template<unsigned int C>
	inline void appendTo(Buffer<C> &b) const
		throw(std::out_of_range)
	{
		unsigned char *p = (unsigned char *)b.appendField(ZT_ADDRESS_LENGTH);
		*(p++) = (unsigned char)((_a >> 32) & 0xff);
		*(p++) = (unsigned char)((_a >> 24) & 0xff);
		*(p++) = (unsigned char)((_a >> 16) & 0xff);
		*(p++) = (unsigned char)((_a >> 8) & 0xff);
		*p = (unsigned char)(_a & 0xff);
	}

	/**
	 * @return Integer containing address (0 to 2^40)
	 */
	inline uint64_t toInt() const
		throw()
	{
		return _a;
	}

	/**
	 * @return Hash code for use with Hashtable
	 */
	inline unsigned long hashCode() const
		throw()
	{
		return (unsigned long)_a;
	}

	/**
	 * @return Hexadecimal string
	 */
	inline std::string toString() const
	{
		char buf[16];
		Utils::snprintf(buf,sizeof(buf),"%.10llx",(unsigned long long)_a);
		return std::string(buf);
	};

	/**
	 * @param buf Buffer to fill
	 * @param len Length of buffer
	 */
	inline void toString(char *buf,unsigned int len) const
	{
		Utils::snprintf(buf,len,"%.10llx",(unsigned long long)_a);
	}

	/**
	 * @return True if this address is not zero
	 */
	inline operator bool() const throw() { return (_a != 0); }

	/**
	 * Set to null/zero
	 */
	inline void zero() throw() { _a = 0; }

	/**
	 * Check if this address is reserved
	 *
	 * The all-zero null address and any address beginning with 0xff are
	 * reserved. (0xff is reserved for future use to designate possibly
	 * longer addresses, addresses based on IPv6 innards, etc.)
	 *
	 * @return True if address is reserved and may not be used
	 */
	inline bool isReserved() const
		throw()
	{
		return ((!_a)||((_a >> 32) == ZT_ADDRESS_RESERVED_PREFIX));
	}

	/**
	 * @param i Value from 0 to 4 (inclusive)
	 * @return Byte at said position (address interpreted in big-endian order)
	 */
	inline unsigned char operator[](unsigned int i) const throw() { return (unsigned char)((_a >> (32 - (i * 8))) & 0xff); }

	inline bool operator==(const uint64_t &a) const throw() { return (_a == (a & 0xffffffffffULL)); }
	inline bool operator!=(const uint64_t &a) const throw() { return (_a != (a & 0xffffffffffULL)); }
	inline bool operator>(const uint64_t &a) const throw() { return (_a > (a & 0xffffffffffULL)); }
	inline bool operator<(const uint64_t &a) const throw() { return (_a < (a & 0xffffffffffULL)); }
	inline bool operator>=(const uint64_t &a) const throw() { return (_a >= (a & 0xffffffffffULL)); }
	inline bool operator<=(const uint64_t &a) const throw() { return (_a <= (a & 0xffffffffffULL)); }

	inline bool operator==(const Address &a) const throw() { return (_a == a._a); }
	inline bool operator!=(const Address &a) const throw() { return (_a != a._a); }
	inline bool operator>(const Address &a) const throw() { return (_a > a._a); }
	inline bool operator<(const Address &a) const throw() { return (_a < a._a); }
	inline bool operator>=(const Address &a) const throw() { return (_a >= a._a); }
	inline bool operator<=(const Address &a) const throw() { return (_a <= a._a); }

private:
	uint64_t _a;
};

} // namespace ZeroTier

#endif
