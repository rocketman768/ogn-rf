/*
    OGN - Open Glider Network - http://glidernet.org/
    Copyright (c) 2015 The OGN Project

    A detailed list of copyright holders can be found in the file "AUTHORS".

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this software.  If not, see <http://www.gnu.org/licenses/>.
*/

// =========================================================================================

#ifndef __SOCKET_H__
#define __SOCKET_H__

#include <stdio.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
// #include <netinet/ip_icmp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifndef __CYGWIN__
#include <sys/sendfile.h>
#endif
#include <fcntl.h>
#include <sys/ioctl.h>

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#ifdef __MACH__
#define MSG_NOSIGNAL 0
#endif


class SocketAddress                    // IP address and port
{ public:

   struct sockaddr_in Address;

  private:

   static const int TmpStringLen = 64;
   char TmpString[TmpStringLen];

  public:

   SocketAddress();

   void Init(void);

   // set IP and port from an ASCII string: "IP:port", IP can be numeric or a host name
   int set(const char *HostColonPort);

   // set only the IP (includes DNS name resolve)
   int setIP(const char *AsciiHost);

   // set the IP from a 32-bit integer
   int setIP(unsigned long IP=INADDR_ANY);

   // get IP as a 32-bit integer
   unsigned long getIP(void) const;

   // is the address defined already ?
   bool isSetup(void) const;

   int setBroadcast(void);

   // set the port
   int setPort(unsigned short Port);

   // get the port
   unsigned short getPort(void) const;

   // get IP as an ASCII string (to print)
   char *getAsciiIP(void) const;

   // get my own host name
   char *getHostName(void);

   static char *getHostName(char *Name, int NameLen);

   // get the "IP:port" ASCII string, IP will be numeric
   char *getIPColonPort(void);

#ifdef __CYGWIN__
#else
   char *getRemoteHostName(void);
#endif

} ;

class SocketBuffer             // data buffer for IP sockets
{ public:

   char *Data;                 //         data storage
   size_t Allocated;           // [bytes] allocated
   size_t Len;                 // [bytes] filled with data
   size_t Done;                // [bytes] processed
   static const size_t AllocUnit = 4096; // allocation step

  public:

   SocketBuffer();
   ~SocketBuffer();

   void Free(void);

   size_t Relocate(size_t Size); // Returns 0 on failure

   int NullTerm(void);        // put null byte at the end, thus it can be treated as a null-terminated string

   void Clear(void);

   bool isDone(void) const;   // is all data processed ?

   int Delete(size_t Ofs, size_t DelLen); // delete some part of the data (involves memory move)

   int SearchLineTerm(int StartIdx=0); // search for a line terminator: \r or \n

   int ReadFromFile(char *FileName);

   int WriteToFile(FILE *File=stdout) const;

   int WriteToFile(const char *FileName) const;

// -----------------------------------------------------------------------------

   int LineLen(size_t Ofs=0, size_t MaxLen=256) const;

   int EOL(size_t Ofs) const;

   int getStatus(void) const;

   int getHeaderLen(void) const;

   int FindTag(const char *Tag, size_t Ofs=0);

   int TagLen(size_t Ofs=0) const;

// -----------------------------------------------------------------------------

} ;

class Socket                   // IP socket
{ public:

   int SocketFile;
   // unsigned long BytesSent, BytesReceived;

  public:

   Socket();
   ~Socket();

   // create a socket
   int Create(int Type=SOCK_STREAM, int Protocol=IPPROTO_TCP);
   int Create_STREAM(void);
   int Create_DGRAM(void);

   int Copy(int NewSocketFile);

   // set connect/read/write to be blocking or not
   int setBlocking(int Block=1);

   int setNonBlocking(void);

   // avoids waiting (in certain cases) till the socket closes completely after the previous server exits
   int setReuseAddress(int Set=1);

   int setKeepAlive(int KeepAlive=1); // keep checking if connection alive while no data is transmitted

   int setLinger(int ON, int Seconds); // gracefull behavior on socket close

   int setNoDelay(int ON=1);

   int setSendBufferSize(int Bytes);

   int getSendBufferSize(void);

   int setReceiveBufferSize(int Bytes);

   int getReceiveBufferSize(void);

/* on Cygwin send and receive timeouts seem to have no effect ...
#ifdef __WINDOWS__
   int setReceiveTimeout(double Seconds) // a blocking receive() will not wait forever
   { long Time = (long)floor(1000*Seconds+0.5);
     return setsockopt(SocketFile, SOL_SOCKET, SO_RCVTIMEO, &Time, sizeof(Time)); }

   int setSendTimeout(double Seconds)   // a blocking send() will not wait forever
   { long Time = (long)floor(1000*Seconds+0.5);
      return setsockopt(SocketFile, SOL_SOCKET, SO_SNDTIMEO, &Time, sizeof(Time)); }
#endif
*/

   int setReceiveTimeout(double Seconds);
   int setSendTimeout(double Seconds);

#if defined(__MACH__) || defined(__CYGWIN__)
#else
   int getMTU(void);
#endif

   int getReceiveQueue(void);

   int getError(void);

   int isListenning(void);

   // listen for incoming UDP connections (become a UDP server)
   int Listen_DGRAM(unsigned short ListenPort);

   // listen for incoming TCP connections (become a TCP server)
   int Listen(unsigned short ListenPort, int MaxConnections=8);

   // accept a new client (when being a server)
   int Accept(Socket &ClientSocket, SocketAddress &ClientAddress);

   // connect to a remote server
   int Connect(SocketAddress &ServerAddress);

   // send data (on a connected socket)
   int Send(void *Message, int Bytes, int Flags=MSG_NOSIGNAL);

   int Send(const char *Message);

   int Send(SocketBuffer &Buffer, int Flags=MSG_NOSIGNAL);

#ifndef __CYGWIN__
   int SendFile(const char *FileName);
#endif

   // send data (on a non-connected socket)
   int SendTo(const void *Message, int Bytes, SocketAddress Address, int Flags=MSG_NOSIGNAL);

   int SendTo(const void *Message, int Bytes, int Flags=MSG_NOSIGNAL);

   // say: I won't send any more data on this connection
   int SendShutdown(void);

#ifndef __CYGWIN__ // Cygwin C++ does not know abour TIOCOUTQ ?
   int getSendQueue(void);
#endif

   // receive data (on a stream socket)
   int Receive(void *Message, int MaxBytes, int Flags=MSG_NOSIGNAL);

   // receive (stream) data into a buffer
   int Receive(SocketBuffer &Buffer, int Flags=MSG_NOSIGNAL);

   // receive data (on a non-connected socket)
   int ReceiveFrom(void *Message, int MaxBytes, SocketAddress &Address, int Flags=MSG_NOSIGNAL);

   // tell if socket is open
   int isOpen(void) const;

   // close the socket
   int Close(void);

   // get the local IP and port
   int getLocalAddress(SocketAddress &Address);

   // get the remote IP and port
   int getRemoteAddress(SocketAddress &Address);

   static void CopyNetToHost(uint32_t *Dst, uint32_t *Src, int Words);

   static void CopyHostoNet(uint32_t *Dst, uint32_t *Src, int Words);
} ;

class UDP_Sender
{ public:
   Socket Sock;
   const static int MaxDest = 4;
   SocketAddress Dest[MaxDest];

  public:
   void ClearDest(void);                                                     // clear the list of destination IP's

   int Open();
   int isOpen() const;
   int Close();
   int setNonBlocking();

   int addDest(const char *Addr);
/*
   int addBroadcast(void)
   { int Idx;
     for( Idx=0; Idx<MaxDest; Idx++)
     { if(Dest[Idx].getIP()==0)
       { if(Dest[Idx].SetIP(INADDR_BROADCAST)<0) return -1;
         return Idx; }
     }
     return -1; }
*/
   void PrintDest(void);

   int Send(uint32_t *Msg, int Words);

   int Send(void *Msg, int Bytes);

   int Receive(void *Msg, int MaxBytes, SocketAddress &Source);

} ;

class UDP_Receiver
{ public:
   Socket Sock;

  public:
   int Open(int Port);
   int Close();
   int setNonBlocking();

   int Receive(void *Msg, int MaxBytes, SocketAddress &Source);

   int Receive(uint32_t *Msg, int MaxWords, SocketAddress &Source);

} ;

// open TCP client connection to a server: return socket file number
int openTCP(const char *HostName, int Port=80);

#endif // of __SOCKET_H__

// =========================================================================================

