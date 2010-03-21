/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2010 The swgANH Team

---------------------------------------------------------------------------------------
*/

#include "SocketReadThread.h"

#include "CompCryptor.h"
#include "NetworkClient.h"
#include "Packet.h"
#include "PacketFactory.h"
#include "Service.h"
#include "Session.h"
#include "SessionFactory.h"
#include "Socket.h"
#include "SocketWriteThread.h"

#include "LogManager/LogManager.h"
#include "Common/MessageFactory.h"

#include <boost/thread/thread.hpp>

#if defined(__GNUC__)
// GCC implements tr1 in the <tr1/*> headers. This does not conform to the TR1
// spec, which requires the header without the tr1/ prefix.
#include <tr1/functional>
#else
#include <functional>
#endif

#if defined(_MSC_VER)
#define socklen_t int
#else
#include <sys/socket.h>
#include <arpa/inet.h>

#define INVALID_SOCKET	-1
#define SOCKET_ERROR	-1
#define closesocket		close
#endif

//======================================================================================================================

SocketReadThread::SocketReadThread(SOCKET socket, SocketWriteThread* writeThread, Service* service,uint32 mfHeapSize, bool serverservice) :
mReceivePacket(0),
mDecompressPacket(0),
mSessionFactory(0),
mPacketFactory(0),
mCompCryptor(0),
mSocket(0),
mIsRunning(false)
{
	if(serverservice)
	{
		mMessageMaxSize = gNetConfig->getServerServerReliableSize();
		mSessionResendWindowSize = gNetConfig->getServerPacketWindow();
	}
	else
	{
		mMessageMaxSize = gNetConfig->getServerClientReliableSize();
		mSessionResendWindowSize = gNetConfig->getClientPacketWindow();
	}

	mSocket = socket;
	mSocketWriteThread = writeThread;

	// Init our NewConnection object
	memset(mNewConnection.mAddress, 0, sizeof(mNewConnection.mAddress));
	mNewConnection.mPort = 0;
	mNewConnection.mSession = 0;

	// Startup our factories
	mMessageFactory = new MessageFactory();
	mMessageFactory->Startup(mfHeapSize,service->getId());

	mPacketFactory = new PacketFactory();
	mPacketFactory->Startup(serverservice);

	mSessionFactory = new SessionFactory();
	mSessionFactory->Startup(writeThread, service, mPacketFactory, mMessageFactory, serverservice);

	mCompCryptor = new CompCryptor();

	// Allocate our receive packets
	mReceivePacket = mPacketFactory->CreatePacket();
	mDecompressPacket = mPacketFactory->CreatePacket();

	// start our thread
    boost::thread t(std::tr1::bind(&SocketReadThread::run, this));
    mThread = boost::move(t);

	HANDLE th =  mThread.native_handle();
	SetPriorityClass(th,REALTIME_PRIORITY_CLASS);	
	//SetPriorityClass(th,NORMAL_PRIORITY_CLASS);	
	
	
}

SocketReadThread::~SocketReadThread()
{
	mExit = true;

    mThread.interrupt();
    mThread.join();

	mPacketFactory->Shutdown();
	mSessionFactory->Shutdown();
	mMessageFactory->Shutdown();

	delete mCompCryptor;
	delete mSessionFactory;
	delete mPacketFactory;
	delete mMessageFactory;
}

//======================================================================================================================

void SocketReadThread::run(void)
{
	struct sockaddr_in  from;
	int              address, fromLen = sizeof(from), count;
	int16               recvLen;
	uint16              port, decompressLen;
	Session*            session;
	fd_set              socketSet;
	struct              timeval tv;

	FD_ZERO(&socketSet);

	// Call our internal _startup method
	_startup();

	while(!mExit)
	{
		// Check to see if *WE* are about to connect to a remote server 
		if(mNewConnection.mPort != 0)
		{
			Session* newSession = mSessionFactory->CreateSession();
			newSession->setCommand(SCOM_Connect);
			newSession->setAddress(inet_addr(mNewConnection.mAddress));
			newSession->setPort(htons(mNewConnection.mPort));
			newSession->setResendWindowSize(mSessionResendWindowSize);

			uint64 hash = newSession->getAddress() | (((uint64)newSession->getPort()) << 32);

			mNewConnection.mSession = newSession;
			mNewConnection.mPort = 0;

			// Add the new session to the main process list

            boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);

			mAddressSessionMap.insert(std::make_pair(hash,newSession));
			mSocketWriteThread->NewSession(newSession);
		}

		// Reset our internal members so we can use the packet again.
		mReceivePacket->Reset();
		mDecompressPacket->Reset();

		// Build a new fd_set structure
		FD_SET(mSocket, &socketSet);

		// We're going to block for 250ms.
		tv.tv_sec   = 0;
		tv.tv_usec  = 50;


		count = select(mSocket, &socketSet, 0, 0, &tv);

		if(count && FD_ISSET(mSocket, &socketSet))
		{

			

			// Read any incoming packets.
			recvLen = recvfrom(mSocket, mReceivePacket->getData(),(int) mMessageMaxSize, 0, (sockaddr*)&from, reinterpret_cast<socklen_t*>(&fromLen));
			if (recvLen <= 2)
			{
				if (recvLen <= 2)
				{
#if(ANH_PLATFORM == ANH_PLATFORM_WIN32)

					int error = WSAGetLastError();
					gLogger->logMsgF("*** Unkown error from socket recvFrom: %i recvL returned %i", MSG_NORMAL, error,recvLen);
					gLogger->logMsgF("*** mMessageMaxSize: %i", MSG_NORMAL, mMessageMaxSize);

#elif(ANH_PLATFORM == ANH_PLATFORM_LINUX)

				//	int error = errno;

#endif
				}
				//if(error != 10040)
				continue;


				recvLen = mMessageMaxSize;
			}
			if(recvLen > mMessageMaxSize)
				gLogger->logMsgF("*** Received Size > mMessageMaxSize: %u", MSG_NORMAL, recvLen);

			// Get our remote Address and port
			address = from.sin_addr.s_addr;
			port = from.sin_port;
			uint64 hash = address | (((uint64)port) << 32);

			// Grab our packet type
			mReceivePacket->Reset();           // Reset our internal members so we can use the packet again.
			mReceivePacket->setSize(recvLen); // crc is subtracted by the decryption
			uint8  packetTypeLow = mReceivePacket->peekUint8();
			uint16 packetType = mReceivePacket->getUint16();

			//gLogger->logMsgF("FromWire, Type:0x%.4x, size:%u, IP:0x%.8x, port:%u", MSG_LOW, packetType, recvLen, address, ntohs(port));


			// TODO: Implement an IP blacklist so we can drop packets immediately.

            //boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);
			//reading should be threadsafe ????
			AddressSessionMap::iterator i = mAddressSessionMap.find(hash);

			if(i != mAddressSessionMap.end())
			{
				session = (*i).second;
			}
			else
			{
				// We should only be creating a new session if it's a session request packet
				if(packetType == SESSIONOP_SessionRequest)
				{
					gLogger->logMsgF("new Session created hash : %I64u ",MSG_HIGH,hash);
					session = mSessionFactory->CreateSession();
					session->setSocketReadThread(this);
					session->setPacketFactory(mPacketFactory);
					session->setAddress(address);  // Store the address and port in network order so we don't have to
					session->setPort(port);  // convert them all the time.  Only convert for humans.
					session->setResendWindowSize(mSessionResendWindowSize);

					// Insert the session into our address map and process list
					boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);
					mAddressSessionMap.insert(std::make_pair(hash, session));
					mSocketWriteThread->NewSession(session);
					session->mHash = hash;

					gLogger->logMsgF("Added Service %i: New Session(%s, %u), AddressMap: %i",MSG_HIGH,mSessionFactory->getService()->getId(), inet_ntoa(from.sin_addr), ntohs(session->getPort()), mAddressSessionMap.size());
				}
				else
				{
					gLogger->logMsgF("*** Session not found.  Packet dropped. Type:0x%.4x", MSG_NORMAL, packetType);

					continue;
				}
			}

			//lk.unlock();

			// I don't like any of the code below, but it's going to take me a bit to work out a good way to handle decompression
			// and decryption.  It's dependent on session layer protocol information, which should not be looked at here.  Should
			// be placed in Session, though I'm not sure how or where yet.
			// Set the size of the packet

			// Validate our date header.  If it's not a valid header, drop it.
			
			if(packetType > 0x00ff && (packetType & 0x00ff) == 0 && session != NULL)
			{
				switch(packetType)
				{
					case SESSIONOP_Disconnect:
					case SESSIONOP_DataAck1:
					case SESSIONOP_DataAck2:
					case SESSIONOP_DataAck3:
					case SESSIONOP_DataAck4:
					case SESSIONOP_DataOrder1:
					case SESSIONOP_DataOrder2:
					case SESSIONOP_DataOrder3:
					case SESSIONOP_DataOrder4:
					case SESSIONOP_Ping:
					{

						// Before we do anythign else, check the CRC.
						uint32 packetCrc = mCompCryptor->GenerateCRC(mReceivePacket->getData(), recvLen - 2, session->getEncryptKey());  // - 2 crc

						uint8 crcLow  = (uint8)*(mReceivePacket->getData() + recvLen - 1);
						uint8 crcHigh = (uint8)*(mReceivePacket->getData() + recvLen - 2);
						//gLogger->logMsgF("checking CRC. key:0x%.8x crc:0x%.4x low:0x%.2x high:0x%.2x len:%u", MSG_LOW, session->getEncryptKey(), packetCrc, crcLow, crcHigh, recvLen);

						if (crcLow != (uint8)packetCrc || crcHigh != (uint8)(packetCrc >> 8))
						{
							// CRC mismatch.  Dropping packet.
							//gLogger->hexDump(mReceivePacket->getData(),mReceivePacket->getSize());
							gLogger->logMsgF("*** Ack or orderr or disconnect or Ping dropped  CRC mismatch.",MSG_NORMAL);
							continue;
						}

						// Decrypt the packet
						mCompCryptor->Decrypt(mReceivePacket->getData() + 2, recvLen - 4, session->getEncryptKey());  // don't hardcode the header buffer or CRc len.

						// Send the packet to the session.
						//gLogger->logMsgF("DIS/ACK/ORDER size:%u",MSG_NORMAL,recvLen);
						session->HandleSessionPacket(mReceivePacket);
						mReceivePacket = mPacketFactory->CreatePacket();
					}
					break;

					case SESSIONOP_MultiPacket:
					case SESSIONOP_NetStatRequest:
					case SESSIONOP_NetStatResponse:
					case SESSIONOP_DataChannel1:
					case SESSIONOP_DataChannel2:
					case SESSIONOP_DataChannel3:
					case SESSIONOP_DataChannel4:
					case SESSIONOP_DataFrag1:
					case SESSIONOP_DataFrag2:
					case SESSIONOP_DataFrag3:
					case SESSIONOP_DataFrag4:
					{

						// Before we do anything else, check the CRC.
						uint32 packetCrc = mCompCryptor->GenerateCRC(mReceivePacket->getData(), recvLen - 2, session->getEncryptKey());

						uint8 crcLow  = (uint8)*(mReceivePacket->getData() + recvLen - 1);
						uint8 crcHigh = (uint8)*(mReceivePacket->getData() + recvLen - 2);

						//gLogger->logMsgF("checking CRC. key:0x%.8x crc:0x%.4x low:0x%.2x high:0x%.2x len:%u", MSG_LOW, session->getEncryptKey(), packetCrc, crcLow, crcHigh, recvLen);

						if (crcLow != (uint8)packetCrc || crcHigh != (uint8)(packetCrc >> 8))
						{
							// CRC mismatch.  Dropping packet.

							gLogger->logMsgF("*** Reliable Packet dropped. %X CRC mismatch.",MSG_NORMAL,packetType);
							mCompCryptor->Decrypt(mReceivePacket->getData() + 2, recvLen - 4, session->getEncryptKey());  // don't hardcode the header buffer or CRC len.

							gLogger->hexDump(mReceivePacket->getData(),mReceivePacket->getSize());
							continue;
						}

						// Decrypt the packet
						mCompCryptor->Decrypt(mReceivePacket->getData() + 2, recvLen - 4, session->getEncryptKey());  // don't hardcode the header buffer or CRC len.

						// Decompress the packet
						decompressLen = mCompCryptor->Decompress(mReceivePacket->getData() + 2, recvLen - 5, mDecompressPacket->getData() + 2, mDecompressPacket->getMaxPayload() - 5);

						if(decompressLen > 0)
						{
							mDecompressPacket->setIsCompressed(true); //tag what? -tmr
							mDecompressPacket->setSize(decompressLen+2); // add the packet header size
							*((uint16*)(mDecompressPacket->getData())) = *((uint16*)mReceivePacket->getData());
							session->HandleSessionPacket(mDecompressPacket);
							mDecompressPacket = mPacketFactory->CreatePacket();

							break;
						}
						else //tag packets can be not compressed --tmr
						{
							mReceivePacket->setSize(mReceivePacket->getSize()-3);//we have to remove comp/crc like the above does natrually -tmr
						}

					}

					case SESSIONOP_SessionRequest:
					case SESSIONOP_SessionResponse:
					case SESSIONOP_FatalError:
					case SESSIONOP_FatalErrorResponse:
					//case SESSIONOP_Reset:
					{
						// Send the packet to the session.
						//gLogger->logMsgF("SESSION size:%u",MSG_NORMAL,recvLen);

						session->HandleSessionPacket(mReceivePacket);
						mReceivePacket = mPacketFactory->CreatePacket();
					}
					break;

					default:
					{
						gLogger->logMsgF("SocketReadThread: Dont know what todo with this packet! --tmr",MSG_NORMAL);
					}
					break;

				} //end switch(sessionOp)
			}
			// Validate that our data is actually fastpath
			// htx, only check the first byte here
			else if(packetTypeLow < 0x0d && session != NULL) //highest fastpath I've seen is 0x0b -tmr
			{
				// Before we do anythign else, check the CRC.
				uint32	packetCrc	= mCompCryptor->GenerateCRC(mReceivePacket->getData(), recvLen - 2, session->getEncryptKey());
				uint8	crcLow		= (uint8)*(mReceivePacket->getData() + recvLen - 1);
				uint8	crcHigh		= (uint8)*(mReceivePacket->getData() + recvLen - 2);

				//gLogger->logMsgF("checking CRC. key:0x%.8x crc:0x%.4x low:0x%.2x high:0x%.2x len:%u", MSG_LOW, session->getEncryptKey(), packetCrc, crcLow, crcHigh, recvLen);

				if(crcLow != (uint8)packetCrc || crcHigh != (uint8)(packetCrc >> 8))
				{
					// CRC mismatch.  Dropping packet.
					gLogger->logMsg("*** Packet dropped.  CRC mismatch.",MSG_NORMAL);
					continue;
				}

				// It's a 'fastpath' packet.  Send it directly up the data channel
				mCompCryptor->Decrypt(mReceivePacket->getData() + 1, recvLen - 3, session->getEncryptKey());  // don't hardcode the header buffer or CRc len.

				// Decompress the packet
				decompressLen	= 0;
				uint8 compFlag	= (uint8)*(mReceivePacket->getData() + recvLen - 3);

				if(compFlag == 1)
				{
					decompressLen = mCompCryptor->Decompress(mReceivePacket->getData() + 1, recvLen - 4, mDecompressPacket->getData() + 1, mDecompressPacket->getMaxPayload() - 4);
				}

				if(decompressLen > 0)
				{
					mDecompressPacket->setIsCompressed(true);
					mDecompressPacket->setSize(decompressLen + 1); // add the packet header size

					*((uint8*)(mDecompressPacket->getData())) = *((uint8*)mReceivePacket->getData());
					// send the packet up the stack
					session->HandleFastpathPacket(mDecompressPacket);
					mDecompressPacket = mPacketFactory->CreatePacket();
				}
				else
				{
					// send the packet up the stack
					mReceivePacket->setSize(mReceivePacket->getSize()-3);//we have to remove comp/crc like the above does natrually -tmr
					session->HandleFastpathPacket(mReceivePacket);
					mReceivePacket = mPacketFactory->CreatePacket();
				}
			}
		}
		boost::this_thread::sleep(boost::posix_time::microseconds(10));

		/*
		boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);
		std::vector<uint64>::iterator it = DestroyList.begin();
		while(it!=DestroyList.end())
		{
			AddressSessionMap::iterator iter = mAddressSessionMap.find((*it));

			if(iter != mAddressSessionMap.end())
			{
				session = (*iter).second;
				mAddressSessionMap.erase(iter);
				gLogger->logMsgF("Service %i: Removing Session(%u), AddressMap: %i hash %I64u",MSG_NORMAL,mSessionFactory->getService()->getId(), ntohs(session->getPort()), mAddressSessionMap.size(),(*it));
				mSessionFactory->DestroySession(session);
				it = DestroyList.erase(it);
			}
			else
			{
				gLogger->logMsgF("SESSION NOT FOUND!!! Service %i: AddressMap: %i hash %I64u SESSION NOT FOUND!!!!!",MSG_NORMAL,mSessionFactory->getService()->getId(), mAddressSessionMap.size(),(*it));
				it++;
			}
			
		}
		*/

	}

	// Shutdown internally
	_shutdown();
}

//======================================================================================================================

void SocketReadThread::NewOutgoingConnection(int8* address, uint16 port)
{
	// This will only handle a single connect call at a time right now.  At some point it would be good to make this a
	// queue so we can process these async.  This is NOT thread safe, and won't be.  Only should be called by the Service.

	// Init our NewConnection object

	strcpy(mNewConnection.mAddress, address);
	mNewConnection.mPort = port;
	mNewConnection.mSession = 0;
}

//======================================================================================================================

void SocketReadThread::RemoveAndDestroySession(Session* session)
{
	// Find and remove the session from the address map.
	// session->getPort() is uint16 !!!!!!!!!!!!!!!!!!!!
	uint64 hash = session->getAddress() | (((uint64)session->getPort()) << 32);

	gLogger->logMsgF("Added to destroy list :::Service %i: Removing Session(%s, %u), AddressMap: %i hash %I64u",MSG_NORMAL,mSessionFactory->getService()->getId(), inet_ntoa(*((in_addr*)(&hash))), ntohs(session->getPort()), mAddressSessionMap.size(),hash);
	gLogger->logMsgF("hash %I64u  vs mHash %I64u",MSG_HIGH,hash,session->mHash);
	
	boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);
	//DestroyList.push_back(session->mHash);
										
	AddressSessionMap::iterator iter = mAddressSessionMap.find(hash);

	if(iter != mAddressSessionMap.end())
	{
		//boost::recursive_mutex::scoped_lock lk(mSocketReadMutex);
		mAddressSessionMap.erase(iter);
		//gLogger->logMsgF("Service %i: Removing Session(%s, %u), AddressMap: %i hash %I64u",MSG_NORMAL,mSessionFactory->getService()->getId(), inet_ntoa(*((in_addr*)(&hash))), ntohs(session->getPort()), mAddressSessionMap.size(),hash);
		mSessionFactory->DestroySession(session);
	}
	else
		gLogger->logMsgF("Service %i: Removing Session FAILED(%s, %u), AddressMap: %i hash %I64u",MSG_NORMAL,mSessionFactory->getService()->getId(), inet_ntoa(*((in_addr*)(&hash))), ntohs(session->getPort()), mAddressSessionMap.size(),hash);

	// why the %/*&+ is this not finding the session ? it completely legitimately finds it in the run() ???
	
}

//======================================================================================================================

void SocketReadThread::_startup(void)
{
	// Initialization is done.  All of it.  :)
	mIsRunning = true;
	mExit = false;
}

//======================================================================================================================

void SocketReadThread::_shutdown(void)
{
	// Shutting down
	mIsRunning = false;
}

//======================================================================================================================





