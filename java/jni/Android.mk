LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := ZeroTierOneJNI
LOCAL_C_INCLUDES := $(ZT1)/include
LOCAL_C_INCLUDES += $(ZT1)/node
LOCAL_LDLIBS := -llog
# LOCAL_CFLAGS := -g

# ZeroTierOne SDK source files
LOCAL_SRC_FILES := \
	$(ZT1)/ext/lz4/lz4.c \
	$(ZT1)/ext/json-parser/json.c \
	$(ZT1)/ext/http-parser/http_parser.c \
	$(ZT1)/node/C25519.cpp \
	$(ZT1)/node/CertificateOfMembership.cpp \
	$(ZT1)/node/DeferredPackets.cpp \
	$(ZT1)/node/Identity.cpp \
	$(ZT1)/node/IncomingPacket.cpp \
	$(ZT1)/node/InetAddress.cpp \
	$(ZT1)/node/Multicaster.cpp \
	$(ZT1)/node/Network.cpp \
	$(ZT1)/node/NetworkConfig.cpp \
	$(ZT1)/node/Node.cpp \
	$(ZT1)/node/OutboundMulticast.cpp \
	$(ZT1)/node/Packet.cpp \
	$(ZT1)/node/Path.cpp \
	$(ZT1)/node/Peer.cpp \
	$(ZT1)/node/Poly1305.cpp \
	$(ZT1)/node/Salsa20.cpp \
	$(ZT1)/node/SelfAwareness.cpp \
	$(ZT1)/node/SHA512.cpp \
	$(ZT1)/node/Switch.cpp \
	$(ZT1)/node/Topology.cpp \
	$(ZT1)/node/Utils.cpp \
	$(ZT1)/osdep/Http.cpp \
	$(ZT1)/osdep/OSUtils.cpp

# JNI Files
LOCAL_SRC_FILES += \
	com_zerotierone_sdk_Node.cpp \
	ZT_jniutils.cpp \
	ZT_jnilookup.cpp

include $(BUILD_SHARED_LIBRARY)