#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <libconfig.h>

#include <ogn-rf/rfacq.h>
#include <ogn-rf/socket.h>
#include <ogn-rf/thread.h>

// Forward declarations
class RF_Acq;
class GSM_FFT;

class HTTP_Server
{
public:
   int      Port;      // listenning port
   Thread   Thr;       // processing thread
   RF_Acq   *RF;        // pointer to RF acquisition
   GSM_FFT  *GSM;
   char     Host[32];  // Host name
   char     ConfigFileName[1024];

   static void *ThreadExec(void *Context);

   HTTP_Server(RF_Acq *RF, GSM_FFT *GSM);
  ~HTTP_Server();

   void Config_Defaults(void);
   int Config(config_t *Config);
   void Start(void);
   void *Exec(void);
   int CopyWord(char *Dst, char *Src, int MaxLen);
   void ProcessRequest(Socket *Client, SocketBuffer &Request);
   void Status(Socket *Client);
};

#endif /*HTTPSERVER_H*/
