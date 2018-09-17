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

#include <ogn-rf/socket.h>

   SocketAddress::SocketAddress()
     { Init(); }

   void SocketAddress::Init(void)
     { Address.sin_family = AF_INET; setIP(); }

   // set IP and port from an ASCII string: "IP:port", IP can be numeric or a host name
   int SocketAddress::set(const char *HostColonPort)
     { const char *Colon=strchr(HostColonPort,':');
       if(Colon==0) return setIP(HostColonPort);
       int Port; if(sscanf(Colon+1,"%d",&Port)!=1) return -1;
       if((Port<0)||(Port>0xFFFF)) return -1;
       setPort((unsigned short)Port);
       int Len=Colon-HostColonPort; if(Len>=TmpStringLen) return -1;
       memcpy(TmpString, HostColonPort, Len); TmpString[Len]='\0';
       int Error = setIP(TmpString);
       return Error; }

   // set only the IP (includes DNS name resolve)
   int SocketAddress::setIP(const char *AsciiHost)
     { in_addr_t IP=inet_addr(AsciiHost);
       if(IP != (in_addr_t)(-1)) { Address.sin_addr.s_addr=IP; return 0; }
       struct hostent *Host = gethostbyname(AsciiHost); if(Host==0) return -1;
       char *AddrPtr=Host->h_addr_list[0];              if(AddrPtr==0) return -1;
       memcpy(&Address.sin_addr, Host->h_addr_list[0],  Host->h_length);
       return 0; }

   // set the IP from a 32-bit integer
   int SocketAddress::setIP(unsigned long IP)
     { Address.sin_addr.s_addr = htonl(IP); return 0; }

   // get IP as a 32-bit integer
   unsigned long SocketAddress::getIP(void) const
     { return ntohl(Address.sin_addr.s_addr); }

   // is the address defined already ?
   bool SocketAddress::isSetup(void) const
     { return getIP()!=INADDR_ANY; }

   int SocketAddress::setBroadcast(void)
     { Address.sin_addr.s_addr = htonl(INADDR_BROADCAST); return 0; }

   // set the port
   int SocketAddress::setPort(unsigned short Port)
     { Address.sin_port = htons(Port); return 0; }

   // get the port
   unsigned short SocketAddress::getPort(void) const
     { return ntohs(Address.sin_port); }

   // get IP as an ASCII string (to print)
   char *SocketAddress::getAsciiIP(void) const
     { return inet_ntoa(Address.sin_addr); }

   // get my own host name
   char *SocketAddress::getHostName(void) { return getHostName(TmpString, TmpStringLen); }

   char *SocketAddress::getHostName(char *Name, int NameLen)
     { if(gethostname(Name, NameLen)<0) return 0;
       return Name; }

   // get the "IP:port" ASCII string, IP will be numeric
   char *SocketAddress::getIPColonPort(void)
     { char *IP = getAsciiIP(); if(IP==0) return IP;
       if(strlen(IP)>(TmpStringLen-8)) return 0;
       unsigned short Port=getPort();
       sprintf(TmpString, "%s:%d", IP, Port);
       return TmpString; }

#ifdef __CYGWIN__
#else
   char *SocketAddress::getRemoteHostName(void)
   { static struct hostent *HostInfo = gethostbyaddr(&Address, sizeof(Address), AF_INET);
     return HostInfo->h_name; }
#endif


   SocketBuffer::SocketBuffer()
     { Data=0; Allocated=0; Len=0; Done=0; }

   SocketBuffer::~SocketBuffer()
     { Free(); }

   void SocketBuffer::Free(void)
     { if(Data) { free(Data); Data=0; }
       Allocated=0; Len=0; Done=0; }

   size_t SocketBuffer::Relocate(size_t Size) // Returns 0 on failure
     { if(Size<=Allocated) return Allocated;
       // printf("Relocate(%d)",Size);
       size_t Units=(Size+AllocUnit-1)/AllocUnit; Size=Units*AllocUnit;
       // printf(" => Units=%d, Size=%d\n", Units, Size);
       Data=(char *)realloc(Data, Size); if(Data==0) Free(); else Allocated=Size;
       return Allocated; }

   int SocketBuffer::NullTerm(void)        // put null byte at the end, thus it can be treated as a null-terminated string
     { if(Relocate(Len+1)<=0) return 0;
       Data[Len]=0;
       return Len; }

   void SocketBuffer::Clear(void)
     { Len=0; Done=0; }

   bool SocketBuffer::isDone(void) const   // is all data processed ?
     { return Done==Len; }

   int SocketBuffer::Delete(size_t Ofs, size_t DelLen) // delete some part of the data (involves memory move)
     { if(Ofs>=Len) return 0;
       if((Ofs+DelLen)>Len) DelLen=Len-Ofs;
       memcpy(Data+Ofs, Data+Ofs+DelLen, Len-DelLen); Len-=DelLen;
       Data[Len]=0; return DelLen; }

   int SocketBuffer::SearchLineTerm(int StartIdx) // search for a line terminator: \r or \n
     { size_t Idx; char Term=0;
       for( Idx=StartIdx; Idx<Len; Idx++)
       { Term=Data[Idx];
         if((Term=='\r')||(Term=='\n')) break; }
       if(Idx>=Len) return -1;        // return -1 if terminator not found
       return Idx-StartIdx; }         // return the line length (not including the terminator)

   int SocketBuffer::ReadFromFile(char *FileName)
     { FILE *File = fopen(FileName,"r"); if(File==0) return -1;
       int Total=0;
       for( ; ; )
       { if(Relocate(Len+AllocUnit)==0) { fclose(File); return -1; }
         int ToRead = Allocated-Len;
         int Read = fread(Data+Len, 1, ToRead, File);
         if(Read<0) { fclose(File); return -1; }
         Len+=Read; Total+=Read; if(Read!=ToRead) break;
       }
       fclose(File);
       return Total; }

   int SocketBuffer::WriteToFile(FILE *File) const
     { int ToWrite = Len-Done; if(ToWrite<0) ToWrite=0;
       int Written = fwrite(Data+Done, 1, ToWrite, File);
       if(Written<0) return Written;
       return Written==ToWrite ? Written:-1; }

   int SocketBuffer::WriteToFile(const char *FileName) const
     { FILE *File = fopen(FileName,"w"); if(File==0) return -1;
       int ToWrite = Len-Done; if(ToWrite<0) ToWrite=0;
       int Written = fwrite(Data+Done, 1, ToWrite, File);
       fclose(File);
       if(Written<0) return Written;
       return Written==ToWrite ? Written:-1; }

// -----------------------------------------------------------------------------

   int SocketBuffer::LineLen(size_t Ofs, size_t MaxLen) const
   { size_t Idx=Ofs;
     for( ; Idx<Len; Idx++)
     { char Byte=Data[Idx]; if( (Byte=='\r') || (Byte=='\n') ) break; }
     return Idx-Ofs; }

   int SocketBuffer::EOL(size_t Ofs) const
   { if(Ofs>=Len) return 0;
     char Byte1=Data[Ofs];
     if( (Byte1!='\r') && (Byte1!='\n') ) return 0;
     Ofs++;
     if(Ofs>=Len) return 1;
     char Byte2=Data[Ofs];
     if( (Byte2!='\r') && (Byte2!='\n') ) return 1;
     if(Byte2==Byte1) return 1;
     return 2; }

   int SocketBuffer::getStatus(void) const
   { char Protocol[16]; int Status=0;
     int FirstLineLen=LineLen(0, 128);
     if(FirstLineLen<=8) return -1;
     if(FirstLineLen>=128) return -1;
     if(sscanf(Data, "%s %d", Protocol, &Status)!=2) return -1;
     return Status; }

   int SocketBuffer::getHeaderLen(void) const
   { size_t Idx=0;
     for( ; ; )
     { int Len=LineLen(Idx);
       int TermLen=EOL(Idx+Len);
       Idx+=Len+TermLen;
       if(Len==0) break; }
     return Idx; }

   int SocketBuffer::FindTag(const char *Tag, size_t Ofs)
   { size_t TagLen=strlen(Tag);
     for(size_t Idx=Ofs; Idx<Len; Idx++)
     { char Byte=Data[Idx]; if(Byte!='<') continue;
       if((Idx+1+TagLen)>=Len) return -1;
       if(memcmp(Tag, Data+Idx+1, TagLen)==0) return Idx-Ofs; }
     return -1; }

   int SocketBuffer::TagLen(size_t Ofs) const
   { for(size_t Idx=Ofs+1; Idx<Len; Idx++)
     { char Byte=Data[Idx]; if(Byte=='>') return Idx-Ofs+1; }
     return -1; }

// -----------------------------------------------------------------------------


   Socket::Socket()
     { SocketFile=(-1); }

   Socket::~Socket()
     { Close(); }

   // create a socket
   int Socket::Create(int Type, int Protocol)
     { Close();
       SocketFile=socket(PF_INET, Type, Protocol);
       return SocketFile; }
   int Socket::Create_STREAM(void) { return Create(SOCK_STREAM, IPPROTO_TCP); }
   int Socket::Create_DGRAM(void) { return Create(SOCK_DGRAM, 0); }

   int Socket::Copy(int NewSocketFile)
     { Close();
       return SocketFile=NewSocketFile; }

   // set connect/read/write to be blocking or not
   int Socket::setBlocking(int Block)
     { int Flags = fcntl(SocketFile,F_GETFL,0);
       if(Block) Flags &= ~O_NONBLOCK;
            else Flags |=  O_NONBLOCK;
       return fcntl(SocketFile,F_SETFL,Flags); }

   int Socket::setNonBlocking(void)
     { return setBlocking(0); }

   // avoids waiting (in certain cases) till the socket closes completely after the previous server exits
   int Socket::setReuseAddress(int Set)
     { return setsockopt(SocketFile, SOL_SOCKET, SO_REUSEADDR, &Set, sizeof(Set)); }

   int Socket::setKeepAlive(int KeepAlive) // keep checking if connection alive while no data is transmitted
     { return setsockopt(SocketFile, SOL_SOCKET, SO_KEEPALIVE, &KeepAlive, sizeof(KeepAlive)); }

   int Socket::setLinger(int ON, int Seconds) // gracefull behavior on socket close
     { struct linger Linger; Linger.l_onoff=ON; Linger.l_linger=Seconds;
       return setsockopt(SocketFile, SOL_SOCKET, SO_LINGER, &Linger, sizeof(Linger)); }

   int Socket::setNoDelay(int ON)
   { return setsockopt(SocketFile, IPPROTO_TCP, TCP_NODELAY, &ON, sizeof(ON)); }

   int Socket::setSendBufferSize(int Bytes)
     { return setsockopt(SocketFile, SOL_SOCKET, SO_SNDBUF, &Bytes, sizeof(Bytes)); }

   int Socket::getSendBufferSize(void)
     { int Bytes=0; socklen_t Size;
       int Error=getsockopt(SocketFile, SOL_SOCKET, SO_SNDBUF, &Bytes, &Size);
       return Error<0 ? -1:Bytes; }

   int Socket::setReceiveBufferSize(int Bytes)
     { return setsockopt(SocketFile, SOL_SOCKET, SO_RCVBUF, &Bytes, sizeof(Bytes)); }

   int Socket::getReceiveBufferSize(void)
     { int Bytes=0; socklen_t Size;
       int Error=getsockopt(SocketFile, SOL_SOCKET, SO_RCVBUF, &Bytes, &Size);
       return Error<0 ? -1:Bytes; }

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

#ifdef __CYGWIN__  // dummy routine for Cygwin, only to satify the compiler
   int Socket::setReceiveTimeout(double Seconds) { return -1; }
   int Socket::setSendTimeout(double Seconds) { return -1; }
#else
   int Socket::setReceiveTimeout(double Seconds) // a blocking receive() will not wait forever
     { struct timeval Time;
       Time.tv_sec  = (long)floor(Seconds);
       Time.tv_usec = (long)floor(1000000*(Seconds-Time.tv_sec)+0.5);
       return setsockopt(SocketFile, SOL_SOCKET, SO_RCVTIMEO, &Time, sizeof(Time)); }

   int Socket::setSendTimeout(double Seconds)   // a blocking send() will not wait forever
     { struct timeval Time;
       Time.tv_sec  = (long)floor(Seconds);
       Time.tv_usec = (long)floor(1000000*(Seconds-Time.tv_sec)+0.5);
       return setsockopt(SocketFile, SOL_SOCKET, SO_SNDTIMEO, &Time, sizeof(Time)); }
#endif

#if defined(__MACH__) || defined(__CYGWIN__)
#else
   int Socket::getMTU(void)
     { int Bytes;
       if(ioctl(SocketFile, SIOCGIFMTU, &Bytes)<0) return -1;
       return Bytes; }
#endif

   int Socket::getReceiveQueue(void)
     { int Bytes;
       if(ioctl(SocketFile, FIONREAD, &Bytes)<0) return -1;
       return Bytes; }

   int Socket::getError(void)
     { int ErrorCode=0;
       socklen_t Size=sizeof(ErrorCode);
       int Error=getsockopt(SocketFile, SOL_SOCKET, SO_ERROR, &ErrorCode, &Size);
       return Error<0 ? -1:ErrorCode; }

   int Socket::isListenning(void)
     { int Yes=0;
       socklen_t Size=sizeof(Yes);
       int Error=getsockopt(SocketFile, SOL_SOCKET, SO_ACCEPTCONN, &Yes, &Size);
       return Error<0 ? -1:Yes; }

   // listen for incoming UDP connections (become a UDP server)
   int Socket::Listen_DGRAM(unsigned short ListenPort)
     { if(SocketFile<0) { if(Create_DGRAM()<0) return -1; }

       setReuseAddress(1);

       struct sockaddr_in ListenAddress;
       ListenAddress.sin_family      = AF_INET;
       ListenAddress.sin_addr.s_addr = htonl(INADDR_ANY);
       ListenAddress.sin_port        = htons(ListenPort);

       if(bind(SocketFile, (struct sockaddr *) &ListenAddress, sizeof(ListenAddress))<0)
       { Close(); return -1; }

       return 0; }

   // listen for incoming TCP connections (become a TCP server)
   int Socket::Listen(unsigned short ListenPort, int MaxConnections)
     { if(SocketFile<0) { if(Create()<0) return -1; }

       setReuseAddress(1);

       struct sockaddr_in ListenAddress;
       ListenAddress.sin_family      = AF_INET;
       ListenAddress.sin_addr.s_addr = htonl(INADDR_ANY);
       ListenAddress.sin_port        = htons(ListenPort);

       if(bind(SocketFile, (struct sockaddr *) &ListenAddress, sizeof(ListenAddress))<0)
       { Close(); return -1; }

       if(listen(SocketFile, MaxConnections)<0)
       { Close(); return -1; }

       return 0; }

   // accept a new client (when being a server)
   int Socket::Accept(Socket &ClientSocket, SocketAddress &ClientAddress)
     { ClientSocket.Close();
       socklen_t ClientAddressLength=sizeof(ClientAddress.Address);
       return ClientSocket.SocketFile=accept(SocketFile, (struct sockaddr *) &(ClientAddress.Address), &ClientAddressLength); }

   // connect to a remote server
   int Socket::Connect(SocketAddress &ServerAddress)
     { if(SocketFile<0) { if(Create_STREAM()<0) return -1; } // if no socket yet, create a STREAM-type one.
       socklen_t ServerAddressLength=sizeof(ServerAddress.Address);
       return connect(SocketFile, (struct sockaddr *) &(ServerAddress.Address), ServerAddressLength); }

   // send data (on a connected socket)
   int Socket::Send(void *Message, int Bytes, int Flags)
     { return send(SocketFile, Message, Bytes, Flags); }

   int Socket::Send(const char *Message)
     { return Send((void *)Message, strlen(Message)); }

   int Socket::Send(SocketBuffer &Buffer, int Flags)
     { size_t Bytes = Buffer.Len-Buffer.Done; // if(Bytes>4096) Bytes=4096;
       int SentBytes=Send(Buffer.Data+Buffer.Done, Bytes, Flags);
       if(SentBytes>0) Buffer.Done+=SentBytes;
       return SentBytes; }

#ifndef __CYGWIN__
   int Socket::SendFile(const char *FileName)
   { int File=open(FileName, O_RDONLY); if(File<0) return File;
     struct stat Stat; fstat(File, &Stat); int Size=Stat.st_size;
     int Ret=sendfile(SocketFile, File, 0, Size);
     close(File);
     return Ret; }
#endif

   // send data (on a non-connected socket)
   int Socket::SendTo(const void *Message, int Bytes, SocketAddress Address, int Flags)
     { socklen_t AddressLength=sizeof(Address.Address);
       return sendto(SocketFile, Message, Bytes, Flags, (struct sockaddr *) &(Address.Address), AddressLength); }

   int Socket::SendTo(const void *Message, int Bytes, int Flags)
     { return sendto(SocketFile, Message, Bytes, Flags, 0, 0); }

   // say: I won't send any more data on this connection
   int Socket::SendShutdown(void)
     { return shutdown(SocketFile, SHUT_WR); }

#ifndef __CYGWIN__ // Cygwin C++ does not know abour TIOCOUTQ ?
   int Socket::getSendQueue(void)
     { int Bytes;
       ioctl(SocketFile, TIOCOUTQ, &Bytes);
       return Bytes; }
#endif

   // receive data (on a stream socket)
   int Socket::Receive(void *Message, int MaxBytes, int Flags)
     { int Len=recv(SocketFile, Message, MaxBytes, Flags);
       if(Len>=0) return Len;
       return errno==EWOULDBLOCK ? 0:Len; }

   // receive (stream) data into a buffer
   int Socket::Receive(SocketBuffer &Buffer, int Flags)
     { size_t NewSize=Buffer.Len+Buffer.AllocUnit/2;
       size_t Allocated=Buffer.Relocate(NewSize);
       int MaxBytes=Allocated-Buffer.Len-1;
       int ReceiveBytes=Receive(Buffer.Data+Buffer.Len, MaxBytes, Flags);
       // printf("Allocated = %d, Receive(%d) => %d\n", Allocated, MaxBytes, ReceiveBytes);
       if(ReceiveBytes>0) { Buffer.Len+=ReceiveBytes; Buffer.Data[Buffer.Len]=0; }
       return ReceiveBytes; }

   // receive data (on a non-connected socket)
   int Socket::ReceiveFrom(void *Message, int MaxBytes, SocketAddress &Address, int Flags)
     { socklen_t AddressLength=sizeof(Address.Address);
       return recvfrom(SocketFile, Message, MaxBytes, Flags, (struct sockaddr *) &(Address.Address), &AddressLength); }

   // tell if socket is open
   int Socket::isOpen(void) const
     { return SocketFile>=0; }

   // close the socket
   int Socket::Close(void)
     { if(SocketFile>=0) close(SocketFile);
       SocketFile=(-1); return 0; }

   // get the local IP and port
   int Socket::getLocalAddress(SocketAddress &Address)
     { socklen_t AddressLength=sizeof(Address.Address);
       return getsockname(SocketFile, (struct sockaddr *) &(Address.Address), &AddressLength); }

   // get the remote IP and port
   int Socket::getRemoteAddress(SocketAddress &Address)
     { socklen_t AddressLength=sizeof(Address.Address);
       return getpeername(SocketFile, (struct sockaddr *) &(Address.Address), &AddressLength); }

   void Socket::CopyNetToHost(uint32_t *Dst, uint32_t *Src, int Words)
   { for( ; Words; Words--) (*Dst++) = ntohl(*Src++); }

   void Socket::CopyHostoNet(uint32_t *Dst, uint32_t *Src, int Words)
   { for( ; Words; Words--) (*Dst++) = htonl(*Src++); }


   void UDP_Sender::ClearDest(void)                                                     // clear the list of destination IP's
   { for(int Idx=0; Idx<MaxDest; Idx++)
     { Dest[Idx].setIP((long unsigned int)0); }
   }

   int UDP_Sender::Open(void)           { ClearDest(); return Sock.Create_DGRAM(); }
   int UDP_Sender::isOpen(void) const   { return Sock.isOpen(); }
   int UDP_Sender::Close(void)          { return Sock.Close(); }
   int UDP_Sender::setNonBlocking(void) { return Sock.setNonBlocking(); }

   int UDP_Sender::addDest(const char *Addr)
   { for(int Idx=0; Idx<MaxDest; Idx++)
     { if(Dest[Idx].getIP()==0)
       { if(Dest[Idx].set(Addr)<0) return -1;
         return Idx; }
     }
     return -1; }
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
   void UDP_Sender::PrintDest(void)
   { printf("Dest[] =");
     for(int Idx=0; Idx<MaxDest; Idx++)
     { if(Dest[Idx].getIP()==0) continue;
       printf(" %s", Dest[Idx].getIPColonPort()); }
     printf("\n");
   }

   int UDP_Sender::Send(uint32_t *Msg, int Words)
   { return Send((void *)Msg, Words*sizeof(uint32_t)); }

   int UDP_Sender::Send(void *Msg, int Bytes)
   { int Count=0;
     for( int Idx=0; Idx<MaxDest; Idx++)
     { if(Dest[Idx].getIP()==0) continue;
       if(Sock.SendTo(Msg, Bytes, Dest[Idx])<0) continue;
       Count++; }
     return Count; }

   int UDP_Sender::Receive(void *Msg, int MaxBytes, SocketAddress &Source)
   { return Sock.ReceiveFrom(Msg, MaxBytes, Source); }

   int UDP_Receiver::Open(int Port)       { return Sock.Listen_DGRAM(Port); }
   int UDP_Receiver::Close(void)          { return Sock.Close(); }
   int UDP_Receiver::setNonBlocking(void) { return Sock.setNonBlocking(); }
   // int getPort(void) const  { return Sock.}

   int UDP_Receiver::Receive(void *Msg, int MaxBytes, SocketAddress &Source)
   { return Sock.ReceiveFrom(Msg, MaxBytes, Source); }

   int UDP_Receiver::Receive(uint32_t *Msg, int MaxWords, SocketAddress &Source)
   { int Words=Sock.ReceiveFrom(Msg, MaxWords*sizeof(uint32_t), Source);
     return Words<0 ? Words:Words/sizeof(uint32_t); }

// open TCP client connection to a server: return socket file number
int openTCP(const char *HostName, int Port)
{ struct hostent *Server = gethostbyname(HostName); if(Server==0) return -1;
  struct sockaddr_in ServerAddr;
  memset(&ServerAddr, 0x00, sizeof(struct sockaddr_in));
  ServerAddr.sin_family = AF_INET;
  ServerAddr.sin_port = htons(Port);
  memcpy(&ServerAddr.sin_addr, Server->h_addr, sizeof(ServerAddr.sin_addr));
  int Socket = socket(AF_INET, SOCK_STREAM, 0); if(Socket<0) return -1;
  return connect(Socket, (struct sockaddr *)&ServerAddr, sizeof(ServerAddr)); }

