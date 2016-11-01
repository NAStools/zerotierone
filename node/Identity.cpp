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
#include <string.h>
#include <stdint.h>

#include "Constants.hpp"
#include "Identity.hpp"
#include "SHA512.hpp"
#include "Salsa20.hpp"
#include "Utils.hpp"

// These can't be changed without a new identity type. They define the
// parameters of the hashcash hashing/searching algorithm.

#define ZT_IDENTITY_GEN_HASHCASH_FIRST_BYTE_LESS_THAN 17
#define ZT_IDENTITY_GEN_MEMORY 2097152

namespace ZeroTier {

// A memory-hard composition of SHA-512 and Salsa20 for hashcash hashing
static inline void _computeMemoryHardHash(const void *publicKey,unsigned int publicKeyBytes,void *digest,void *genmem)
{
	// Digest publicKey[] to obtain initial digest
	SHA512::hash(digest,publicKey,publicKeyBytes);

	// Initialize genmem[] using Salsa20 in a CBC-like configuration since
	// ordinary Salsa20 is randomly seekable. This is good for a cipher
	// but is not what we want for sequential memory-harndess.
	memset(genmem,0,ZT_IDENTITY_GEN_MEMORY);
	Salsa20 s20(digest,256,(char *)digest + 32);
	s20.encrypt20((char *)genmem,(char *)genmem,64);
	for(unsigned long i=64;i<ZT_IDENTITY_GEN_MEMORY;i+=64) {
		unsigned long k = i - 64;
		*((uint64_t *)((char *)genmem + i)) = *((uint64_t *)((char *)genmem + k));
		*((uint64_t *)((char *)genmem + i + 8)) = *((uint64_t *)((char *)genmem + k + 8));
		*((uint64_t *)((char *)genmem + i + 16)) = *((uint64_t *)((char *)genmem + k + 16));
		*((uint64_t *)((char *)genmem + i + 24)) = *((uint64_t *)((char *)genmem + k + 24));
		*((uint64_t *)((char *)genmem + i + 32)) = *((uint64_t *)((char *)genmem + k + 32));
		*((uint64_t *)((char *)genmem + i + 40)) = *((uint64_t *)((char *)genmem + k + 40));
		*((uint64_t *)((char *)genmem + i + 48)) = *((uint64_t *)((char *)genmem + k + 48));
		*((uint64_t *)((char *)genmem + i + 56)) = *((uint64_t *)((char *)genmem + k + 56));
		s20.encrypt20((char *)genmem + i,(char *)genmem + i,64);
	}

	// Render final digest using genmem as a lookup table
	for(unsigned long i=0;i<(ZT_IDENTITY_GEN_MEMORY / sizeof(uint64_t));) {
		unsigned long idx1 = (unsigned long)(Utils::ntoh(((uint64_t *)genmem)[i++]) % (64 / sizeof(uint64_t)));
		unsigned long idx2 = (unsigned long)(Utils::ntoh(((uint64_t *)genmem)[i++]) % (ZT_IDENTITY_GEN_MEMORY / sizeof(uint64_t)));
		uint64_t tmp = ((uint64_t *)genmem)[idx2];
		((uint64_t *)genmem)[idx2] = ((uint64_t *)digest)[idx1];
		((uint64_t *)digest)[idx1] = tmp;
		s20.encrypt20(digest,digest,64);
	}
}

// Hashcash generation halting condition -- halt when first byte is less than
// threshold value.
struct _Identity_generate_cond
{
	_Identity_generate_cond() throw() {}
	_Identity_generate_cond(unsigned char *sb,char *gm) throw() : digest(sb),genmem(gm) {}
	inline bool operator()(const C25519::Pair &kp) const
		throw()
	{
		_computeMemoryHardHash(kp.pub.data,(unsigned int)kp.pub.size(),digest,genmem);
		return (digest[0] < ZT_IDENTITY_GEN_HASHCASH_FIRST_BYTE_LESS_THAN);
	}
	unsigned char *digest;
	char *genmem;
};

void Identity::generate()
{
	unsigned char digest[64];
	char *genmem = new char[ZT_IDENTITY_GEN_MEMORY];

	C25519::Pair kp;
	do {
		kp = C25519::generateSatisfying(_Identity_generate_cond(digest,genmem));
		_address.setTo(digest + 59,ZT_ADDRESS_LENGTH); // last 5 bytes are address
	} while (_address.isReserved());

	_publicKey = kp.pub;
	if (!_privateKey)
		_privateKey = new C25519::Private();
	*_privateKey = kp.priv;

	delete [] genmem;
}

bool Identity::locallyValidate() const
{
	if (_address.isReserved())
		return false;

	unsigned char digest[64];
	char *genmem = new char[ZT_IDENTITY_GEN_MEMORY];
	_computeMemoryHardHash(_publicKey.data,(unsigned int)_publicKey.size(),digest,genmem);
	delete [] genmem;

	unsigned char addrb[5];
	_address.copyTo(addrb,5);

	return (
		(digest[0] < ZT_IDENTITY_GEN_HASHCASH_FIRST_BYTE_LESS_THAN)&&
		(digest[59] == addrb[0])&&
		(digest[60] == addrb[1])&&
		(digest[61] == addrb[2])&&
		(digest[62] == addrb[3])&&
		(digest[63] == addrb[4]));
}

std::string Identity::toString(bool includePrivate) const
{
	std::string r;

	r.append(_address.toString());
	r.append(":0:"); // 0 == IDENTITY_TYPE_C25519
	r.append(Utils::hex(_publicKey.data,(unsigned int)_publicKey.size()));
	if ((_privateKey)&&(includePrivate)) {
		r.push_back(':');
		r.append(Utils::hex(_privateKey->data,(unsigned int)_privateKey->size()));
	}

	return r;
}

bool Identity::fromString(const char *str)
{
	if (!str)
		return false;

	char *saveptr = (char *)0;
	char tmp[1024];
	if (!Utils::scopy(tmp,sizeof(tmp),str))
		return false;

	delete _privateKey;
	_privateKey = (C25519::Private *)0;

	int fno = 0;
	for(char *f=Utils::stok(tmp,":",&saveptr);(f);f=Utils::stok((char *)0,":",&saveptr)) {
		switch(fno++) {
			case 0:
				_address = Address(f);
				if (_address.isReserved())
					return false;
				break;
			case 1:
				if ((f[0] != '0')||(f[1]))
					return false;
				break;
			case 2:
				if (Utils::unhex(f,_publicKey.data,(unsigned int)_publicKey.size()) != _publicKey.size())
					return false;
				break;
			case 3:
				_privateKey = new C25519::Private();
				if (Utils::unhex(f,_privateKey->data,(unsigned int)_privateKey->size()) != _privateKey->size())
					return false;
				break;
			default:
				return false;
		}
	}
	if (fno < 3)
		return false;

	return true;
}

} // namespace ZeroTier
