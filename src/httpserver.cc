#include <ogn-rf/httpserver.h>

#include <ogn-rf/sysmon.h>

#define QUOTE(name) #name
#define STR(macro) QUOTE(macro)

HTTP_Server::HTTP_Server(RF_Acq *RF, GSM_FFT *GSM)
   { this->RF=RF; this->GSM=GSM;
     Host[0]=0; SocketAddress::getHostName(Host, 32);
     Config_Defaults(); }

   void HTTP_Server::Config_Defaults(void)
   { ConfigFileName[0]=0;
     Port=8080; }

   int HTTP_Server::Config(config_t *Config)
   { config_lookup_int(Config, "HTTP.Port", &Port); return 0; }

   void HTTP_Server::Start(void)
   { if(Port<=0) return;
     Thr.setExec(ThreadExec); Thr.Create(this); }

  HTTP_Server::~HTTP_Server()
   { if(Port) Thr.Cancel(); }

   void *HTTP_Server::ThreadExec(void *Context)
   { HTTP_Server *This = (HTTP_Server *)Context; return This->Exec(); }

   void *HTTP_Server::Exec(void)
   { printf("HTTP_Server.Exec() ... Start\n");
     while(1)
     { Socket Listen;
       // if(Listen.Create_STREAM()<0) { printf("HTTP_Server.Exec() ... Cannot Create_STREAM()\n"); sleep(1); continue; }
       // if(Listen.setReuseAddress()<0) { printf("HTTP_Server.Exec() ... Cannot setReuseAddress()\n"); sleep(1); continue; }
       if(Listen.Listen(Port)<0) { printf("HTTP_Server.Exec() ... Cannot listen() on port %d\n", Port); sleep(1); continue; }
       printf("HTTP_Server.Exec() ... Listening on port %d\n", Port);
       while(1)
       { Socket *Client = new Socket; SocketAddress ClientAddress;
         if(Listen.Accept(*Client, ClientAddress)<0) { printf("HTTP_Server.Exec() ... Cannot accept()\n"); sleep(1); break; }
         printf("HTTP_Server.Exec() ... Client from %s\n", ClientAddress.getIPColonPort());
         Client->setReceiveTimeout(2.0); Client->setSendTimeout(20.0); Client->setLinger(1, 5);
         SocketBuffer Request; time_t ConnectTime; time(&ConnectTime);
         while(1)
         { if(Client->Receive(Request)<0) { printf("HTTP_Server.Exec() ... Cannot receive()\n"); Client->SendShutdown(); Client->Close(); delete Client; Client=0; break; }
           if( Request.Len && strstr(Request.Data, "\r\n\r\n") ) break;
           time_t Now; time(&Now);
           if((Now-ConnectTime)>2) { printf("HTTP_Server.Exec() ... Request timeout\n"); Client->SendShutdown(); Client->Close(); delete Client; Client=0; break; }
         }
         if(Client)
         { // printf("HTTP_Server.Exec() ... Request[%d]:\n", Request.Len); Request.WriteToFile(stdout); fflush(stdout);
           ProcessRequest(Client, Request); }
       }
       Listen.Close();
     }
     printf("HTTP_Server.Exec() ... Stop\n");
     return 0; }

   int HTTP_Server::CopyWord(char *Dst, char *Src, int MaxLen)
   { int Count=0; MaxLen-=1;
     for( ; ; )
     { char ch = (*Src++); if(ch<=' ') break;
       if(Count>=MaxLen) return -1;
       (*Dst++) = ch; Count++; }
     (*Dst++)=0;
     return Count; }

   void HTTP_Server::ProcessRequest(Socket *Client, SocketBuffer &Request)
   { if(memcmp(Request.Data, "GET ", 4)!=0) goto BadRequest;
     char File[64]; if(CopyWord(File, Request.Data+4, 64)<0) goto BadRequest;
     printf("HTTP_Server.Exec() ... Request for %s\n", File);

          if(strcmp(File, "/")==0)
     { Status(Client); return; }
     else if( (strcmp(File, "/status.html")==0)         || (strcmp(File, "status.html")==0) )
     { Status(Client); return; }
     else if( (strcmp(File, "/gsm-spectrogram.jpg")==0) || (strcmp(File, "gsm-spectrogram.jpg")==0) )
     { GSM->SpectrogramQueue.Push(Client); return; }
     else if( (strcmp(File, "/spectrogram.jpg")==0) || (strcmp(File, "spectrogram.jpg")==0) )
     { RF->SpectrogramQueue.Push(Client); return; }
     else if( (strcmp(File, "/time-slot-rf.u8")==0)  || (strcmp(File, "time-slot-rf.u8")==0) )
     { RF->RawDataQueue.Push(Client); return; }
     // NotFound:
       Client->Send("HTTP/1.0 404 Not Found\r\n\r\n"); Client->SendShutdown(); Client->Close(); delete Client; return;

     BadRequest:
       Client->Send("HTTP/1.0 400 Bad Request\r\n\r\n"); Client->SendShutdown(); Client->Close(); delete Client; return;
   }

   void HTTP_Server::Status(Socket *Client)
   { Client->Send("\
HTTP/1.1 200 OK\r\n\
Cache-Control: no-cache\r\n\
Content-Type: text/html\r\n\
Refresh: 5\r\n\
\r\n\
<!DOCTYPE html>\r\n\
<html>\r\n\
");
     // time_t Now; time(&Now);
     dprintf(Client->SocketFile, "\
<title>%s RTLSDR-OGN RF processor " STR(VERSION) " status</title>\n\
<b>RTLSDR OGN RF processor " STR(VERSION) "/"__DATE__"</b><br /><br />\n\n", Host);

     dprintf(Client->SocketFile, "<table>\n<tr><th>System</th><th></th></tr>\n");

     dprintf(Client->SocketFile, "<tr><td>Host name</td><td align=right><b>%s</b></td></tr>\n", Host);
     dprintf(Client->SocketFile, "<tr><td>Configuration file path+name</td><td align=right><b>%s</b></td></tr>\n", ConfigFileName);
     time_t Now; time(&Now);
     struct tm TM; localtime_r(&Now, &TM);
     dprintf(Client->SocketFile, "<tr><td>Local time</td><td align=right><b>%02d:%02d:%02d</b></td></tr>\n", TM.tm_hour, TM.tm_min, TM.tm_sec);
     dprintf(Client->SocketFile, "<tr><td>Software</td><td align=right><b>" STR(VERSION) "</b></td></tr>\n");

#ifndef __MACH__
     struct sysinfo SysInfo;
     if(sysinfo(&SysInfo)>=0)
     { dprintf(Client->SocketFile, "<tr><td>CPU load</td><td align=right><b>%3.1f/%3.1f/%3.1f</b></td></tr>\n",
                                   SysInfo.loads[0]/65536.0, SysInfo.loads[1]/65536.0, SysInfo.loads[2]/65536.0);
       dprintf(Client->SocketFile, "<tr><td>RAM [free/total]</td><td align=right><b>%3.1f/%3.1f MB</b></td></tr>\n",
                                   1e-6*SysInfo.freeram*SysInfo.mem_unit, 1e-6*SysInfo.totalram*SysInfo.mem_unit);
     }
#endif

     float CPU_Temperature;
     if(getCpuTemperature(CPU_Temperature)>=0)
       dprintf(Client->SocketFile, "<tr><td>CPU temperature</td><td align=right><b>%+5.1f &#x2103;</b></td></tr>\n",    CPU_Temperature);
     float SupplyVoltage=0;
     if(getSupplyVoltage(SupplyVoltage)>=0)
       dprintf(Client->SocketFile, "<tr><td>Supply voltage</td><td align=right><b>%5.3f V</b></td></tr>\n",    SupplyVoltage);
     float SupplyCurrent=0;
     if(getSupplyCurrent(SupplyCurrent)>=0)
       dprintf(Client->SocketFile, "<tr><td>Supply current</td><td align=right><b>%5.3f A</b></td></tr>\n",    SupplyCurrent);

     double NtpTime, EstError, RefFreqCorr;
     if(getNTP(NtpTime, EstError, RefFreqCorr)>=0)
     { time_t Time = floor(NtpTime);
       struct tm TM; gmtime_r(&Time, &TM);
       dprintf(Client->SocketFile, "<tr><td>NTP UTC time</td><td align=right><b>%02d:%02d:%02d</b></td></tr>\n", TM.tm_hour, TM.tm_min, TM.tm_sec);
       dprintf(Client->SocketFile, "<tr><td>NTP est. error</td><td align=right><b>%3.1f ms</b></td></tr>\n", 1e3*EstError);
       dprintf(Client->SocketFile, "<tr><td>NTP freq. corr.</td><td align=right><b>%+5.2f ppm</b></td></tr>\n", RefFreqCorr);
     }

     if(RF->SDR.isOpen())
     { dprintf(Client->SocketFile, "<tr><th>RTL-SDR device #%d</th><th></th></tr>\n",                                  RF->SDR.DeviceIndex);
       dprintf(Client->SocketFile, "<tr><td>Name</td><td align=right><b>%s</b></td></tr>\n",                           RF->SDR.getDeviceName());
       dprintf(Client->SocketFile, "<tr><td>Tuner type</td><td align=right><b>%s</b></td></tr>\n",                     RF->SDR.getTunerTypeName());
       char Manuf[256], Product[256], Serial[256];
       RF->SDR.getUsbStrings(Manuf, Product, Serial);
       dprintf(Client->SocketFile, "<tr><td>Manufacturer</td><td align=right><b>%s</b></td></tr>\n",                    Manuf);
       dprintf(Client->SocketFile, "<tr><td>Product</td><td align=right><b>%s</b></td></tr>\n",                         Product);
       dprintf(Client->SocketFile, "<tr><td>Serial</td><td align=right><b>%s</b></td></tr>\n",                          Serial);
#ifdef NEW_RTLSDR_LIB
       for(int Stage=0; Stage<8; Stage++)
       { char Descr[256]; int Gains=RF->SDR.getTunerStageGains(Stage, 0, Descr); if(Gains<=0) break;
         dprintf(Client->SocketFile, "<tr><td>Tuner stage #%d</td><td align=right><b>%s [%2d]</b></td></tr>\n",  Stage, Descr, Gains);
       }
       dprintf(Client->SocketFile, "<tr><td>Tuner bandwidths</td><td align=right><b>[%d]</b></td></tr>\n",             RF->SDR.getTunerBandwidths());
       dprintf(Client->SocketFile, "<tr><td>Tuner gains</td><td align=right><b>[%d]</b></td></tr>\n",                  RF->SDR.getTunerGains());
#endif
       dprintf(Client->SocketFile, "<tr><td>Center frequency</td><td align=right><b>%7.3f MHz</b></td></tr>\n",        1e-6*RF->SDR.getCenterFreq());
       dprintf(Client->SocketFile, "<tr><td>Sample rate</td><td align=right><b>%5.3f MHz</b></td></tr>\n",             1e-6*RF->SDR.getSampleRate());
       dprintf(Client->SocketFile, "<tr><td>Frequency correction</td><td align=right><b>%+5.1f ppm</b></td></tr>\n",   RF->FreqCorr + RF->GSM_FreqCorr);
       dprintf(Client->SocketFile, "<tr><td>Live Time</td><td align=right><b>%5.1f%%</b></td></tr>\n",         100*RF->getLifeTime());
       uint32_t RtlFreq, TunerFreq; RF->SDR.getXtalFreq(RtlFreq, TunerFreq);
       dprintf(Client->SocketFile, "<tr><td>RTL Xtal</td><td align=right><b>%8.6f MHz</b></td></tr>\n",                1e-6*RtlFreq);
       dprintf(Client->SocketFile, "<tr><td>Tuner Xtal</td><td align=right><b>%8.6f MHz</b></td></tr>\n",              1e-6*TunerFreq);
/*
       dprintf(Client->SocketFile, "<tr><td>Gain[%d]</td><td align=right><b>", RF->SDR.Gains);
       for(int Idx=0; Idx<RF->SDR.Gains; Idx++)
       { dprintf(Client->SocketFile, "%c%3.1f", Idx?',':' ', 0.1*RF->SDR.Gain[Idx]);
       }
       dprintf(Client->SocketFile, " [dB]</b></td></tr>\n");
*/
     }

     dprintf(Client->SocketFile, "<tr><th>RF</th><th></th></tr>\n");
     dprintf(Client->SocketFile, "<tr><td>RF.FreqPlan</td><td align=right><b>%d: %s</b></td></tr>\n",   RF->HoppingPlan.Plan, RF->HoppingPlan.getPlanName() );
     dprintf(Client->SocketFile, "<tr><td>RF.Device</td><td align=right><b>%d</b></td></tr>\n",                       RF->DeviceIndex);
     if(RF->DeviceSerial[0])
       dprintf(Client->SocketFile, "<tr><td>RF.DeviceSerial</td><td align=right><b>%s</b></td></tr>\n",               RF->DeviceSerial);
     dprintf(Client->SocketFile, "<tr><td>RF.SampleRate</td><td align=right><b>%3.1f MHz</b></td></tr>\n",       1e-6*RF->SampleRate);
     // dprintf(Client->SocketFile, "<tr><td>RF.PipeName</td><td align=right><b>%s</b></td></tr>\n",                  ??->OutPipeName );
     dprintf(Client->SocketFile, "<tr><td>RF.FreqCorr</td><td align=right><b>%+3d ppm</b></td></tr>\n",              RF->FreqCorr);
     if(RF->FreqRaster)
       dprintf(Client->SocketFile, "<tr><td>RF.FreqRaster</td><td align=right><b>%3d Hz</b></td></tr>\n",          RF->FreqRaster);
     if(RF->BiasTee>=0)
       dprintf(Client->SocketFile, "<tr><td>RF.BiasTee</td><td align=right><b>%d</b></td></tr>\n",                    RF->BiasTee);
     dprintf(Client->SocketFile, "<tr><td>RF.OffsetTuning</td><td align=right><b>%d</b></td></tr>\n",                 RF->OffsetTuning);
     dprintf(Client->SocketFile, "<tr><td>Fine calib. FreqCorr</td><td align=right><b>%+5.1f ppm</b></td></tr>\n",    RF->GSM_FreqCorr);
     dprintf(Client->SocketFile, "<tr><td>RF.PulseFilter.Threshold</td><td align=right><b>%d</b></td></tr>\n",        RF->PulseFilt.Threshold);
     dprintf(Client->SocketFile, "<tr><td>RF.PulseFilter duty</td><td align=right><b>%5.1f ppm</b></td></tr>\n",    1e6*RF->PulseFilt.Duty);
     // dprintf(Client->SocketFile, "<tr><td>RF.ToneFilter.Enable</td><td align=right><b>%d</b></td></tr>\n",                  FFT->Filter->Enable);
     // dprintf(Client->SocketFile, "<tr><td>RF.ToneFilter.FFTsize</td><td align=right><b>%d</b></td></tr>\n",                 FFT->Filter->FFTsize);
     // dprintf(Client->SocketFile, "<tr><td>RF.ToneFilter.Threshold</td><td align=right><b>%3.1f</b></td></tr>\n",            FFT->Filter->Threshold);
     dprintf(Client->SocketFile, "<tr><td>RF.OGN.GainMode</td><td align=right><b>%d</b></td></tr>\n",                 RF->OGN_GainMode);
     dprintf(Client->SocketFile, "<tr><td>RF.OGN.Gain</td><td align=right><b>%4.1f dB</b></td></tr>\n",           0.1*RF->OGN_Gain);
     // dprintf(Client->SocketFile, "<tr><td>RF.OGN.CenterFreq</td><td align=right><b>%5.1f MHz</b></td></tr>\n",   1e-6*RF->OGN_CenterFreq);
     // dprintf(Client->SocketFile, "<tr><td>RF.OGN.FreqHopChannels</td><td align=right><b>%d</b></td></tr>\n",          RF->OGN_FreqHopChannels);
     dprintf(Client->SocketFile, "<tr><td>RF.OGN.StartTime</td><td align=right><b>%5.3f sec</b></td></tr>\n",         RF->OGN_StartTime);
     dprintf(Client->SocketFile, "<tr><td>RF.OGN.SensTime</td><td align=right><b>%5.3f sec</b></td></tr>\n", (double)(RF->OGN_SamplesPerRead)/RF->SampleRate);
     dprintf(Client->SocketFile, "<tr><td>RF.OGN.SaveRawData</td><td align=right><b>%d sec</b></td></tr>\n", RF->OGN_SaveRawData);
     dprintf(Client->SocketFile, "<tr><td>RF.GSM.CenterFreq</td><td align=right><b>%5.1f MHz</b></td></tr>\n",   1e-6*RF->GSM_CenterFreq);
     dprintf(Client->SocketFile, "<tr><td>RF.GSM.Scan</td><td align=right><b>%d</b></td></tr>\n",                     RF->GSM_Scan);
     dprintf(Client->SocketFile, "<tr><td>RF.GSM.Gain</td><td align=right><b>%4.1f dB</b></td></tr>\n",           0.1*RF->GSM_Gain);
     dprintf(Client->SocketFile, "<tr><td>RF.GSM.SensTime</td><td align=right><b>%5.3f sec</b></td></tr>\n", (double)(RF->GSM_SamplesPerRead)/RF->SampleRate);


     dprintf(Client->SocketFile, "</table>\n");

     Client->Send("\
<br />\r\n\
RF spectrograms:\r\n\
<a href='spectrogram.jpg'>OGN</a><br />\r\n\
<a href='gsm-spectrogram.jpg'>GSM frequency calibration</a><br />\r\n\
<br /><br />\r\n\
<a href='time-slot-rf.u8'>RF raw data</a> of a time-slot (8-bit unsigned I/Q) - a 2 MB binary file !<br />\r\n\
");

     Client->Send("</html>\r\n");
     Client->SendShutdown(); Client->Close(); delete Client; }
