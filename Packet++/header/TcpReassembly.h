#ifndef PACKETPP_TCP_REASSEMBLY
#define PACKETPP_TCP_REASSEMBLY

#include "Packet.h"
#include "IpAddress.h"
#include "PointerVector.h"
#include <map>
#include <list>
#include <time.h>


/**
 * @file
 * This is an implementation of TCP reassembly logic, which means reassembly of TCP messages spanning multiple TCP segments (or packets).<BR>
 * This logic can be useful in analyzing messages for a large number of protocols implemented on top of TCP including HTTP, SSL/TLS, FTP and many many more.
 *
 * __General Features:__
 * - Manage multiple TCP connections under one pcpp#TcpReassembly instance
 * - Support TCP retransmission
 * - Support out-of-order packets
 * - Support missing TCP data
 * - TCP connections can end "naturally" (by FIN/RST packets) or manually by the user
 * - Support callbacks for new TCP data, connection start and connection end
 *
 * __Logic Description:__
 * - The user creates an instance of the pcpp#TcpReassembly class
 * - Then the user starts feeding it with TCP packets
 * - The pcpp#TcpReassembly instance manages all TCP connections from the packets it's being fed. For each connection it manages its 2 sides (A->B and B->A)
 * - When a packet arrives, it is first classified to a certain TCP connection
 * - Then it is classified to a certain side of the TCP connection
 * - Then the pcpp#TcpReassembly logic tries to understand if the data in this packet is the expected data (sequence-wise) and if it's new (e.g isn't a retransmission)
 * - If the packet data matches these criteria a callback is being invoked. This callback is supplied by the user in the creation of the pcpp#TcpReassembly instance. This callback contains
 *   the new data (of course), but also information about the connection (5-tuple, 4-byte hash key describing the connection, etc.) and also a pointer to a "user cookie", meaning a pointer to
 *   a structure provided by the user during the creation of the pcpp#TcpReassembly instance
 * - If the data in this packet isn't new, it's being ignored
 * - If the data in this packet isn't expected (meaning this packet came out-of-order), then the data is being queued internally and will be sent to the user when its turn arrives
 *   (meaning, after the data before arrives)
 * - If the missing data doesn't arrive until a new message from the other side of the connection arrives or until the connection ends - this will be considered as missing data and the
 *   queued data will be sent to the user, but the string "[X bytes missing]" will be added to the message sent in the callback
 * - pcpp#TcpReassembly supports 2 more callbacks - one is invoked when a new TCP connection is first seen and the other when it's ended (either by a FIN/RST packet or manually by the user).
 *   Both of these callbacks contain data about the connection (5-tuple, 4-byte hash key describing the connection, etc.) and also a pointer to a "user cookie", meaning a pointer to a
 *   structure provided by the user during the creation of the pcpp#TcpReassembly instance. The end connection callback also provides the reason for closing it ("naturally" or manually)
 *
 * __Basic Usage and APIs:__
 * - pcpp#TcpReassembly c'tor - Create an instance, provide the callbacks and the user cookie to the instance
 * - pcpp#TcpReassembly#reassemblePacket() - Feed pcpp#TcpReassembly instance with packets
 * - pcpp#TcpReassembly#closeConnection() - Manually close a connection by a flow key
 * - pcpp#TcpReassembly#closeAllConnections() - Manually close all currently opened connections
 * - pcpp#TcpReassembly#OnTcpMessageReady callback - Invoked when new data arrives on a certain connection. Contains the new data as well as connection data (5-tuple, flow key)
 * - pcpp#TcpReassembly#OnTcpConnectionStart callback - Invoked when a new connection is identified
 * - pcpp#TcpReassembly#OnTcpConnectionEnd callback - Invoked when a connection ends (either by FIN/RST or manually by the user)
 *
 * __Additional information:__
 * When the connection is closed the information is not being deleted from memory immediately. There is a delay between these moments. Existence of this delay is caused by two reasons:
 * - pcpp#TcpReassembly#reassemblePacket() should detect the packets that arrive after the FIN packet has been received
 * - the user can use the information about connections managed by pcpp#TcpReassembly instance. Following methods are used for this purpose: pcpp#TcpReassembly#getConnectionInformation and pcpp#TcpReassembly#isConnectionOpen.
 * Cleaning of memory can be performed automatically (the default behavior) by pcpp#TcpReassembly#reassemblePacket() or manually by calling pcpp#TcpReassembly#purgeClosedConnections in the user code.
 * Automatic cleaning is performed once per second.
 *
 * The struct pcpp#TcpReassemblyConfiguration allows to setup the parameters of cleanup. Following parameters are supported:
 * - pcpp#TcpReassemblyConfiguration#doNotRemoveConnInfo - if this member is set to false the automatic cleanup mode is applied
 * - pcpp#TcpReassemblyConfiguration#closedConnectionDelay - the value of delay expressed in seconds. The minimum value is 1
 * - pcpp#TcpReassemblyConfiguration#maxNumToClean - to avoid performance overhead when the cleanup is being performed, this parameter is used. It defines the maximum number of items to be removed per one call of pcpp#TcpReassembly#purgeClosedConnections
 *
 */

/**
 * @namespace pcpp
 * @brief The main namespace for the PcapPlusPlus lib
 */
namespace pcpp
{

/**
 * @struct ConnectionData
 * Represents basic TCP/UDP + IP connection data
 */
struct ConnectionData
{
	/** Source IP address */
	IPAddress* srcIP;
	/** Destination IP address */
	IPAddress* dstIP;
	/** Source TCP/UDP port */
	size_t srcPort;
	/** Destination TCP/UDP port */
	size_t dstPort;
	/** A 4-byte hash key representing the connection */
	uint32_t flowKey;
	/** Start TimeStamp of the connection */
	timeval startTime;
	/** End TimeStamp of the connection */
	timeval endTime;

	/**
	 * A c'tor for this struct that basically zeros all members
	 */
	ConnectionData() : srcIP(NULL), dstIP(NULL), srcPort(0), dstPort(0), flowKey(0), startTime(), endTime()  {}

	/**
	 * A d'tor for this strcut. Notice it frees the memory of srcIP and dstIP members
	 */
	~ConnectionData();

	/**
	 * A copy constructor for this struct. Notice it clones ConnectionData#srcIP and ConnectionData#dstIP
	 */
	ConnectionData(const ConnectionData& other);

	/**
	 * An assignment operator for this struct. Notice it clones ConnectionData#srcIP and ConnectionData#dstIP
	 */
	ConnectionData& operator=(const ConnectionData& other);

	/**
	 * Set source IP
	 * @param[in] sourceIP A pointer to the source IP to set. Notice the IPAddress object will be cloned
	 */
	void setSrcIpAddress(const IPAddress* sourceIP) { srcIP = sourceIP->clone(); }

	/**
	 * Set destination IP
	 * @param[in] destIP A pointer to the destination IP to set. Notice the IPAddress object will be cloned
	 */
	void setDstIpAddress(const IPAddress* destIP) { dstIP = destIP->clone(); }

	/**
	 * Set startTime of Connection
	 * @param[in] startTime integer value
	 */
	void setStartTime(const timeval &startTime) { this->startTime = startTime; }

	/**
	 * Set endTime of Connection
	 * @param[in] endTime integer value
	 */
	void setEndTime(const timeval &endTime) { this->endTime = endTime; }

private:

	void copyData(const ConnectionData& other);
};


class TcpReassembly;


/**
 * @class TcpStreamData
 * When following a TCP connection each packet may contain a piece of the data transferred between the client and the server. This class represents these pieces: each instance of it
 * contains a piece of data, usually extracted from a single packet, as well as information about the connection
 */
class TcpStreamData
{
	friend class TcpReassembly;

public:

	/**
	 * A c'tor for this class that basically zeros all members
	 */
	TcpStreamData();

	/**
	 * A c'tor for this class that get data from outside and set the internal members. Notice that when this class is destroyed it also frees the TCP data it stores
	 * @param[in] tcpData A buffer containing the TCP data piece
	 * @param[in] tcpDataLength The length of the buffer
	 * @param[in] connData TCP connection information for this TCP data
	 */
	TcpStreamData(uint8_t* tcpData, size_t tcpDataLength, ConnectionData connData);

	/**
	 * A d'tor for this class
	 */
	~TcpStreamData();

	/**
	 * A copy c'tor for this class. Notice the data buffer is copied from the source instance to this instance, so even if the source instance is destroyed the data in this instance
	 * stays valid. When this instance is destroyed it also frees the data buffer
	 * @param[in] other The instance to copy from
	 */
	TcpStreamData(TcpStreamData& other);

	/**
	 * Overload of the assignment operator. Notice the data buffer is copied from the source instance to this instance, so even if the source instance is destroyed the data in this instance
	 * stays valid. When this instance is destroyed it also frees the data buffer
	 * @param[in] other The instance to copy from
	 * @return A reference to this instance
	 */
	TcpStreamData& operator=(const TcpStreamData& other);

	/**
	 * A getter for the data buffer
	 * @return A pointer to the buffer
	 */
	uint8_t* getData() const { return m_Data; }

	/**
	 * A getter for buffer length
	 * @return Buffer length
	 */
	size_t getDataLength() const { return m_DataLen; }

	/**
	 * A getter for the connection data
	 * @return The connection data
	 */
	ConnectionData getConnectionData() const { return m_Connection; }

	/**
	 * A getter for the connection data
	 * @return The const reference to connection data
	 */
	const ConnectionData& getConnectionDataRef() const { return m_Connection; }

private:
	uint8_t* m_Data;
	size_t m_DataLen;
	ConnectionData m_Connection;
	bool m_DeleteDataOnDestruction;

	void setDeleteDataOnDestruction(bool flag) { m_DeleteDataOnDestruction = flag; }
	void copyData(const TcpStreamData& other);
};


/**
 * @struct TcpReassemblyConfiguration
 * A structure for configuring the TcpReassembly class
 */
struct TcpReassemblyConfiguration
{
	/** The flag indicating whether to remove the connection data after a connection is closed */
	bool removeConnInfo;

	/** How long the closed connections will not be cleaned up. The value is expressed in seconds. If the value is set to 0 then TcpReassembly should use the default value.
	 * This parameter is only relevant if removeConnInfo is equal to true.
	 */
	uint32_t closedConnectionDelay;

	/** The maximum number of items to be cleaned up per one call of purgeClosedConnections. If the value is set to 0 then TcpReassembly should use the default value.
	 * This parameter is only relevant if removeConnInfo is equal to true.
	 */
	uint32_t maxNumToClean;

	/**
	 * A c'tor for this struct
	 * @param[in] removeConnInfo The flag indicating whether to remove the connection data after a connection is closed. The default is true
	 * @param[in] closedConnectionDelay How long the closed connections will not be cleaned up. The value is expressed in seconds. If it's set to 0 the default value will be used. The default is 5.
	 * @param[in] maxNumToClean The maximum number of items to be cleaned up per one call of purgeClosedConnections. If it's set to 0 the default value will be used. The default is 30.
	 */
	TcpReassemblyConfiguration(bool removeConnInfo = true, uint32_t closedConnectionDelay = 5, uint32_t maxNumToClean = 30) :
		removeConnInfo(removeConnInfo), closedConnectionDelay(closedConnectionDelay), maxNumToClean(maxNumToClean)
	{
	}
};


/**
 * @class TcpReassembly
 * A class containing the TCP reassembly logic. Please refer to the documentation at the top of TcpReassembly.h for understanding how to use this class
 */
class TcpReassembly
{
public:

	/**
	 * An enum for connection end reasons
	 */
	enum ConnectionEndReason
	{
		/** Connection ended because of FIN or RST packet */
		TcpReassemblyConnectionClosedByFIN_RST,
		/** Connection ended manually by the user */
		TcpReassemblyConnectionClosedManually
	};

	/**
	 * The type for storing the connection information
	 */
	typedef std::map<uint32_t, ConnectionData> ConnectionInfoList;

	/**
	 * @typedef OnTcpMessageReady
	 * A callback invoked when new data arrives on a connection
	 * @param[in] side The side this data belongs to (MachineA->MachineB or vice versa). The value is 0 or 1 where 0 is the first side seen in the connection and 1 is the second side seen
	 * @param[in] tcpData The TCP data itself + connection information
	 * @param[in] userCookie A pointer to the cookie provided by the user in TcpReassembly c'tor (or NULL if no cookie provided)
	 */
	typedef void (*OnTcpMessageReady)(int side, TcpStreamData tcpData, void* userCookie);

	/**
	 * @typedef OnTcpConnectionStart
	 * A callback invoked when a new TCP connection is identified (whether it begins with a SYN packet or not)
	 * @param[in] connectionData Connection information
	 * @param[in] userCookie A pointer to the cookie provided by the user in TcpReassembly c'tor (or NULL if no cookie provided)
	 */
	typedef void (*OnTcpConnectionStart)(ConnectionData connectionData, void* userCookie);

	/**
	 * @typedef OnTcpConnectionEnd
	 * A callback invoked when a TCP connection is terminated, either by a FIN or RST packet or manually by the user
	 * @param[in] connectionData Connection information
	 * @param[in] reason The reason for connection termination: FIN/RST packet or manually by the user
	 * @param[in] userCookie A pointer to the cookie provided by the user in TcpReassembly c'tor (or NULL if no cookie provided)
	 */
	typedef void (*OnTcpConnectionEnd)(ConnectionData connectionData, ConnectionEndReason reason, void* userCookie);

	/**
	 * A c'tor for this class
	 * @param[in] onMessageReadyCallback The callback to be invoked when new data arrives
	 * @param[in] userCookie A pointer to an object provided by the user. This pointer will be returned when invoking the various callbacks. This parameter is optional, default cookie is NULL
	 * @param[in] onConnectionStartCallback The callback to be invoked when a new connection is identified. This parameter is optional
	 * @param[in] onConnectionEndCallback The callback to be invoked when a new connection is terminated (either by a FIN/RST packet or manually by the user). This parameter is optional
	 * @param[in] config Optional parameter for defining special configuration parameters. If not set the default parameters will be set
	 */
	TcpReassembly(OnTcpMessageReady onMessageReadyCallback, void* userCookie = NULL, OnTcpConnectionStart onConnectionStartCallback = NULL, OnTcpConnectionEnd onConnectionEndCallback = NULL, const TcpReassemblyConfiguration &config = TcpReassemblyConfiguration());

	/**
	 * A d'tor for this class. Frees all internal structures. Notice that if the d'tor is called while connections are still open, all data is lost and TcpReassembly#OnTcpConnectionEnd won't
	 * be called for those connections
	 */
	~TcpReassembly();

	/**
	 * The most important method of this class which gets a packet from the user and processes it. If this packet opens a new connection, ends a connection or contains new data on an
	 * existing connection, the relevant callback will be called (TcpReassembly#OnTcpMessageReady, TcpReassembly#OnTcpConnectionStart, TcpReassembly#OnTcpConnectionEnd)
	 * @param[in] tcpData A reference to the packet to process
	 */
	void reassemblePacket(Packet& tcpData);

	/**
	 * The most important method of this class which gets a raw packet from the user and processes it. If this packet opens a new connection, ends a connection or contains new data on an
	 * existing connection, the relevant callback will be invoked (TcpReassembly#OnTcpMessageReady, TcpReassembly#OnTcpConnectionStart, TcpReassembly#OnTcpConnectionEnd)
	 * @param[in] tcpRawData A reference to the raw packet to process
	 */
	void reassemblePacket(RawPacket* tcpRawData);

	/**
	 * Close a connection manually. If the connection doesn't exist or already closed an error log is printed. This method will cause the TcpReassembly#OnTcpConnectionEnd to be invoked with
	 * a reason of TcpReassembly#TcpReassemblyConnectionClosedManually
	 * @param[in] flowKey A 4-byte hash key representing the connection. Can be taken from a ConnectionData instance
	 */
	void closeConnection(uint32_t flowKey);

	/**
	 * Close all open connections manually. This method will cause the TcpReassembly#OnTcpConnectionEnd to be invoked for each connection with a reason of
	 * TcpReassembly#TcpReassemblyConnectionClosedManually
	 */
	void closeAllConnections();

	/**
	 * Get a map of all connections managed by this TcpReassembly instance (both connections that are open and those that are already closed)
	 * @return A map of all connections managed. Notice this map is constant and cannot be changed by the user
	 */
	const ConnectionInfoList &getConnectionInformation() const { return m_ConnectionInfo; }

	/**
	 * Check if a certain connection managed by this TcpReassembly instance is currently opened or closed
	 * @param[in] connection The connection to check
	 * @return A positive number (> 0) if connection is opened, zero (0) if connection is closed, and a negative number (< 0) if this connection isn't managed by this TcpReassembly instance
	 */
	int isConnectionOpen(const ConnectionData& connection) const;

	/**
	 * Clean up the closed connections from the memory
	 * @param[in] maxNumToClean The maximum number of items to be cleaned up per one call. This parameter, when its value is not zero, overrides the value that was set by the constructor.
	 * @return The number of cleared items
	 */
	uint32_t purgeClosedConnections(uint32_t maxNumToClean = 0);

private:
	struct TcpFragment
	{
		uint32_t sequence;
		size_t dataLength;
		uint8_t* data;

		TcpFragment() { sequence = 0; dataLength = 0; data = NULL; }
		~TcpFragment() { if (data != NULL) delete [] data; }
	};

	struct TcpOneSideData
	{
		IPAddress* srcIP;
		uint16_t srcPort;
		uint32_t sequence;
		PointerVector<TcpFragment> tcpFragmentList;
		bool gotFinOrRst;

		void setSrcIP(IPAddress* sourrcIP);

		TcpOneSideData() { srcIP = NULL; srcPort = 0; sequence = 0; gotFinOrRst = false; }

		~TcpOneSideData() { if (srcIP != NULL) delete srcIP; }
	};

	struct TcpReassemblyData
	{
		int numOfSides;
		int prevSide;
		TcpOneSideData twoSides[2];
		ConnectionData connData;

		TcpReassemblyData() { numOfSides = 0; prevSide = -1; }
	};
	
	typedef std::map<uint32_t, TcpReassemblyData *> ConnectionList;
	typedef std::map<time_t, std::list<uint32_t> > CleanupList;

	OnTcpMessageReady m_OnMessageReadyCallback;
	OnTcpConnectionStart m_OnConnStart;
	OnTcpConnectionEnd m_OnConnEnd;
	void* m_UserCookie;
	ConnectionList m_ConnectionList;
	ConnectionInfoList m_ConnectionInfo;
	CleanupList m_CleanupList;
	bool m_RemoveConnInfo;
	uint32_t m_ClosedConnectionDelay;
	uint32_t m_MaxNumToClean;
	time_t m_PurgeTimepoint;

	void checkOutOfOrderFragments(TcpReassemblyData* tcpReassemblyData, int sideIndex, bool cleanWholeFragList);

	std::string prepareMissingDataMessage(uint32_t missingDataLen);

	void handleFinOrRst(TcpReassemblyData* tcpReassemblyData, int sideIndex, uint32_t flowKey);

	void closeConnectionInternal(uint32_t flowKey, ConnectionEndReason reason);

	void insertIntoCleanupList(uint32_t flowKey);
};

}

#endif /* PACKETPP_TCP_REASSEMBLY */
