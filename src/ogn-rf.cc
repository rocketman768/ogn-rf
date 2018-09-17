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

#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <libconfig.h>

#include <algorithm>

#include <ogn-rf/thread.h>     // multi-thread stuff
#include <ogn-rf/fft.h>        // Fast Fourier Transform
#include <ogn-rf/gsmfft.h>
#include <ogn-rf/httpserver.h>
#include <ogn-rf/rtlsdr.h>     // SDR radio
#include <ogn-rf/rfacq.h>

#ifndef VERSION
#define VERSION 0.0.0
#endif

#include <ogn-rf/freqplan.h>

#include <ogn-rf/jpeg.h>
#include <ogn-rf/socket.h>
#include <ogn-rf/sysmon.h>

#include <ogn-rf/pulsefilter.h>
#include <ogn-rf/tonefilter.h>

#include <ogn-rf/dataserver.h>

// ==================================================================================================

 class Inp_Filter
{ public:
   typedef float Float;
   Thread Thr;                                      // processing thread
   volatile int StopReq;
   RF_Acq *RF;

   int              Enable;
   ToneFilter<Float> ToneFilt;

   ReuseObjectQueue< SampleBuffer< std::complex<Float> > > OutQueue;

  public:

   Inp_Filter(RF_Acq *RF)
   { this->RF=RF; Config_Defaults(); Preset(); }

   void Config_Defaults(void)
   { Enable  = 0; ToneFilt.FFTsize = 32768; ToneFilt.Threshold=32; }

   int Config(config_t *Config)
   { config_lookup_int(Config,   "RF.ToneFilter.Enable",    &Enable);
     config_lookup_int(Config,   "RF.ToneFilter.FFTsize",   &ToneFilt.FFTsize);
     config_lookup_float(Config, "RF.ToneFilter.Threshold", &ToneFilt.Threshold);
     return 0; }

   int Preset(void) { return ToneFilt.Preset(); }

   int QueueSize(void) { return OutQueue.Size(); }
   void Start(void)
   { StopReq=0; Thr.setExec(ThreadExec); Thr.Create(this); }

  ~Inp_Filter()
   { Thr.Cancel(); }

   double getCPU(void) // get CPU time for this thread
   {
#if !defined(__MACH__)
       struct timespec now; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now); return now.tv_sec + 1e-9*now.tv_nsec;
#else
       return 0;
#endif
   }

   static void *ThreadExec(void *Context)
   { Inp_Filter *This = (Inp_Filter *)Context; return This->Exec(); }

   void *Exec(void)
   { // printf("Inp_Filter.Exec() ... Start\n");
     while(!StopReq)
     { if(!Enable) { sleep(1); continue; }
       double ExecTime=getCPU();
       SampleBuffer<uint8_t> *InpBuffer = RF->OutQueue.Pop();   // here we wait for a new data batch
       // printf("Inp_Filter.Exec() ... Input(%5.3fMHz, %5.3fsec, %dsamples)\n", 1e-6*InpBuffer->Freq, InpBuffer->Time, InpBuffer->Full/2);
       SampleBuffer< std::complex<Float> > *OutBuffer = OutQueue.New();
       ToneFilt.Process(OutBuffer, InpBuffer);
       RF->OutQueue.Recycle(InpBuffer);                         // let the input buffer go free
       // printf("Inp_Filter.Exec() ... Output(%5.3fMHz, %5.3fsec, %dsamples)\n", 1e-6*OutBuffer->Freq, OutBuffer->Time, OutBuffer->Full/2);
       if(OutQueue.Size()<4) { OutQueue.Push(OutBuffer); }
                        else { OutQueue.Recycle(OutBuffer); printf("Inp_Filter.Exec() ... Dropped a slot\n"); }
       ExecTime=getCPU()-ExecTime; // printf("Inp_FFT.Exec() ... %5.3fsec\n", ExecTime);
     }
     // printf("Inp_FFT.Exec() ... Stop\n");
     return 0; }

   // classical sliding box filter - calc. the sum within box of 2*Radius+1
   static void BoxSum(Float *Output, Float *Input, int Size, int Radius)
   { int BoxSize=2*Radius+1;
     Float Sum=0; int InpIdx=0; int OutIdx=0;
     for( ; InpIdx<Radius; InpIdx++)
     { Sum+=Input[InpIdx]; }
     for( ; InpIdx<BoxSize; InpIdx++)
     { Sum+=Input[InpIdx]; Output[OutIdx++]=Sum; }
     for( ; InpIdx<Size; InpIdx++)
     { Sum+=Input[InpIdx]-Input[InpIdx-BoxSize]; Output[OutIdx++]=Sum; }
     for( ; OutIdx<Size; )
     { Sum-=Input[InpIdx-BoxSize]; Output[OutIdx++]=Sum; }
   }


} ;

// ==================================================================================================

class Inp_FFT                                       // FFT of the RF data
{ public:
   typedef float Float;
   Thread Thr;                                      // processing thread
   volatile int StopReq;
   RF_Acq *RF;
   Inp_Filter *Filter;

   int              FFTsize;
#ifdef USE_RPI_GPU_FFT
   RPI_GPU_FFT      FFT;
#else
   DFT1d<Float>     FFT;
#endif
   Float           *Window;

   SampleBuffer< std::complex<Float> > OutBuffer;

   char OutPipeName[32];                            // name of the pipe to send the RF data (as FFT) to the demodulator and decoder.
   int  OutPipe;
   TCP_DataServer DataServer;
   const static uint32_t OutPipeSync = 0x254F7D00 + sizeof(Float);

  public:
   Inp_FFT(RF_Acq *RF, Inp_Filter *Filter=0)
   { Window=0; this->RF=RF; this->Filter=Filter; Preset(); OutPipe=(-1); Config_Defaults(); }

   void Config_Defaults(void)
   { strcpy(OutPipeName, "ogn-rf.fifo"); }

   int Config(config_t *Config)
   { const char *PipeName = "ogn-rf.fifo";
     config_lookup_string(Config, "RF.PipeName",   &PipeName);
     strcpy(OutPipeName, PipeName);
     return 0; }

  int Preset(void) { return Preset(RF->SampleRate); }
   int Preset(int SampleRate)
   { FFTsize=(8*8*SampleRate)/15625;
     FFT.PresetForward(FFTsize);
     Window=(Float *)realloc(Window, FFTsize*sizeof(Float));
     FFT.SetSineWindow(Window, FFTsize, (Float)(1.0/sqrt(FFTsize)) );
     return 1; }

  int SerializeSpectra(int OutPipe)
  {          int Len=Serialize_WriteSync(OutPipe, OutPipeSync);
    if(Len>=0) { Len=Serialize_WriteName(OutPipe, "FreqCorr"); }
    if(Len>=0) { Len=Serialize_WriteData(OutPipe, (void *)&(RF->FreqCorr),     sizeof(int)   ); }
    if(Len>=0) { Len=Serialize_WriteData(OutPipe, (void *)&(RF->GSM_FreqCorr), sizeof(float) ); }
    if(Len>=0) { Len=Serialize_WriteSync(OutPipe, OutPipeSync); }
    if(Len>=0) { Len=Serialize_WriteName(OutPipe, "Spectra"); }
    if(Len>=0) { Len=OutBuffer.Serialize(OutPipe); }
    return Len; }

  int WriteToPipe(void) // write OutBuffer to the output pipe
  { if( (OutPipe<0) && (!DataServer.isListenning()) )
    { const char *Colon=strchr(OutPipeName, ':');
      if(Colon)
      { int Port=atoi(Colon+1);
        if(DataServer.Listen(Port)<0)
          printf("Inp_FFT.Exec() ... cannot open data server on port %d\n", Port);
        else
          printf("Inp_FFT.Exec() ... data server listenning on port %d\n", Port);
      }
      else
      { OutPipe=open(OutPipeName, O_WRONLY);
        if(OutPipe<0)
        { printf("Inp_FFT.Exec() ... Cannot open %s\n", OutPipeName);
          // here we could try to create the missing pipe
          if(mkfifo(OutPipeName, 0666)<0)
            printf("Inp_FFT.Exec() ... Cannot create %s\n", OutPipeName);
          else
          { printf("Inp_FFT.Exec() ... %s has been created\n", OutPipeName);
            OutPipe=open(OutPipeName, O_WRONLY); }
        }
      }
      if( (OutPipe<0) && (!DataServer.isListenning()) ) return -1;
    }
    if(DataServer.isListenning())
    { for(int Idx=0; Idx<DataServer.Clients(); Idx++)                                // loop over clients
      { int Len=SerializeSpectra(DataServer.Client[Idx]);                            // serialize same data to every client
        if(Len<0)
        { printf("Inp_FFT.Exec() ... Dropped a client\n");
          DataServer.Close(Idx); }                                                   // if anything goes wrong: close this client
      }
      int Ret=DataServer.RemoveClosed();                                             // remove closed clients from the list
      Ret=DataServer.Accept();             	                                     // check for more clients who might be waiting to connect
      if(Ret>0) printf("Inp_FFT.Exec() ... Accepted new client (%d clients now)\n", DataServer.Clients() );
    }
    if(OutPipe>=0)
    { int Len=SerializeSpectra(OutPipe);
      if(Len<0) { printf("Inp_FFT.Exec() ... Error while writing to %s\n", OutPipeName); close(OutPipe); OutPipe=(-1); return -1; }
    }
    return 0; }

   void Start(void)
   { StopReq=0; Thr.setExec(ThreadExec); Thr.Create(this); }

  ~Inp_FFT()
   { Thr.Cancel();
     if(Window) free(Window); }

   double getCPU(void) // get CPU time for this thread
   {
#if !defined(__MACH__)
       struct timespec now; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now); return now.tv_sec + 1e-9*now.tv_nsec;
#else
       return 0;
#endif
   }

   static void *ThreadExec(void *Context)
   { Inp_FFT *This = (Inp_FFT *)Context; return This->Exec(); }

   void *Exec(void)
   { // printf("Inp_FFT.Exec() ... Start\n");
     while(!StopReq)
     { double ExecTime=getCPU();
#ifndef USE_RPI_GPU_FFT
       if(Filter && Filter->Enable)
       { SampleBuffer< std::complex<Float> > *InpBuffer = Filter->OutQueue.Pop();
         // printf("Inp_FFT.Exec() ... (%5.3fMHz, %5.3fsec, %dsamples)\n", 1e-6*InpBuffer->Freq, InpBuffer->Time, InpBuffer->Full/2);
         SlidingFFT(OutBuffer, *InpBuffer, FFT, Window);  // Process input samples, produce FFT spectra
         Filter->OutQueue.Recycle(InpBuffer);
       }
       else
#endif
       { SampleBuffer<uint8_t> *InpBuffer = RF->OutQueue.Pop(); // here we wait for a new data batch
         // printf("Inp_FFT.Exec() ... (%5.3fMHz, %5.3fsec, %dsamples)\n", 1e-6*InpBuffer->Freq, InpBuffer->Time, InpBuffer->Full/2);
         SlidingFFT(OutBuffer, *InpBuffer, FFT, Window);  // Process input samples, produce FFT spectra
         RF->OutQueue.Recycle(InpBuffer);
       }
       WriteToPipe(); // here we send the FFT spectra in OutBuffer to the demodulator
       ExecTime=getCPU()-ExecTime; // printf("Inp_FFT.Exec() ... %5.3fsec\n", ExecTime);
     }
     // printf("Inp_FFT.Exec() ... Stop\n");
     if(OutPipe>=0) { close(OutPipe); OutPipe=(-1); }
     return 0; }

} ;

// ==================================================================================================

  RF_Acq      RF;                         // RF input: acquires RF data for OGN and selected GSM frequency

  Inp_Filter  Filter(&RF);                // Coherent interference filter

  Inp_FFT     FFT(&RF, &Filter);          // FFT for OGN demodulator
  GSM_FFT     GSM(&RF);                   // GSM frequency calibration

  HTTP_Server HTTP(&RF, &GSM);            // HTTP server to show status and spectrograms

void SigHandler(int signum) // Signal handler, when user pressed Ctrl-C or process stops for whatever reason
{ RF.StopReq=1; }

// ----------------------------------------------------------------------------------------------------

int SetUserValue(const char *Name, float Value)
{ // printf("%s = %f\n", Name, Value);
  if(strcmp(Name, "RF.FreqCorr")==0)
  { RF.FreqCorr=(int)floor(Value+0.5);
    printf("RF.FreqCorr=%+d ppm\n", RF.FreqCorr);
    return 1; }
  if(strcmp(Name, "RF.OGN.Gain")==0)
  { RF.OGN_Gain=(int)floor(10*Value+0.5);
    printf("RF.OGN.Gain=%3.1f dB\n", 0.1*RF.OGN_Gain);
    return 1; }
  if(strcmp(Name, "RF.OGN.GainMode")==0)
  { RF.OGN_GainMode=(int)floor(Value+0.5);
    printf("RF.OGN.GainMode=%d\n", RF.OGN_GainMode);
    return 1; }
  if(strcmp(Name, "RF.OGN.SaveRawData")==0)
  { RF.OGN_SaveRawData=(int)floor(Value+0.5);
    printf("RF.OGN.SaveRawData=%d\n", RF.OGN_SaveRawData);
    return 1; }
  if(strcmp(Name, "RF.GSM.Gain")==0)
  { RF.GSM_Gain=(int)floor(10*Value+0.5);
    printf("RF.GSM.Gain=%3.1f dB\n", 0.1*RF.GSM_Gain);
    return 1; }
  if(strcmp(Name, "RF.GSM.Scan")==0)
  { RF.GSM_Scan=(int)floor(Value+0.5);
    printf("RF.GSM.Scan=%d\n", RF.GSM_Scan);
    return 1; }
  // if(strcmp(Name, "RF.OGN.CenterFreq")==0)
  // { RF.OGN_CenterFreq=(int)floor(1e6*Value+0.5);
  //   printf("RF.OGN.CenterFreq=%7.3f MHz\n", 1e-6*RF.OGN_CenterFreq);
  //   return 1; }
  if(strcmp(Name, "RF.GSM.CenterFreq")==0)
  { RF.GSM_CenterFreq=(int)floor(1e6*Value+0.5);
    printf("RF.GSM.CenterFreq=%7.3f MHz\n", 1e-6*RF.GSM_CenterFreq);
    return 1; }
  // if(strcmp(Name, "RF.OGN.FreqHopChannels")==0)
  // { RF.OGN_FreqHopChannels=(int)floor(Value+0.5);
  //   printf("RF.OGN_FreqHopChannels=%d\n", RF.OGN_FreqHopChannels);
  //   return 1; }
  if(strcmp(Name, "RF.PulseFilter.Threshold")==0)
  { RF.PulseFilt.Threshold=(int)floor(Value+0.5);
    printf("RF.PulseFilter.Threshold=%d\n", RF.PulseFilt.Threshold);
    return 1; }
  return 0; }

int PrintUserValues(void)
{ printf("Settable parameters:\n");
  printf("RF.FreqCorr=%+d(%+3.1f) ppm\n",      RF.FreqCorr, RF.GSM_FreqCorr);
  printf("RF.PulseFilter.Threshold=%d",        RF.PulseFilt.Threshold);
  if(RF.PulseFilt.Threshold) printf(" .Duty=%3.1fppm", 1e6*RF.PulseFilt.Duty);
  printf("\n");
  // printf("RF.OGN.CenterFreq=%7.3f MHz\n", 1e-6*RF.OGN_CenterFreq);
  // printf("RF.OGN_FreqHopChannels=%d\n",        RF.OGN_FreqHopChannels);
  printf("RF.OGN.Gain=%3.1f dB\n",         0.1*RF.OGN_Gain);
  printf("RF.OGN.GainMode=%d\n",               RF.OGN_GainMode);
  printf("RF.OGN.SaveRawData=%d\n",            RF.OGN_SaveRawData);
  printf("RF.GSM.CenterFreq=%7.3f MHz\n", 1e-6*RF.GSM_CenterFreq);
  printf("RF.GSM.Scan=%d\n",                   RF.GSM_Scan);
  printf("RF.GSM.Gain=%3.1f dB\n",         0.1*RF.GSM_Gain);
  return 0; }

int UserCommand(char *Cmd)
{ if(strchr(Cmd, '\n')==0) return 0;
  char *Equal = strchr(Cmd, '=');
  // printf("User command: %s", Cmd);
  if(Equal)
  { Equal[0]=0; const char *ValuePtr=Equal+1;
    char *Name=Cmd;
    for( ; ; )
    { char ch=Name[0]; if(ch==0) break;
      if(ch>' ') break;
      Name++; }
    for( ; ; )
    { Equal--;
      char ch=Equal[0]; if(ch==0) break;
      if(ch>' ') break;
      Equal[0]=0; }
    float Value;
    if(sscanf(ValuePtr, "%f", &Value)==1) SetUserValue(Name, Value);
  }
  PrintUserValues();
  return 0; }

// ----------------------------------------------------------------------------------------------------

int main(int argc, char *argv[])
{
  const char *ConfigFileName = "rtlsdr-ogn.conf";
  if(argc>1) ConfigFileName = argv[1];

  config_t Config;
  config_init(&Config);
  if(config_read_file(&Config, ConfigFileName)==CONFIG_FALSE)
  { printf("Could not read %s as configuration file\n", ConfigFileName); config_destroy(&Config); return -1; }

  struct sigaction SigAction;
  SigAction.sa_handler = SigHandler;              // setup the signal handler (for Ctrl-C or when process is stopped)
  sigemptyset(&SigAction.sa_mask);
  SigAction.sa_flags = 0;

  struct sigaction SigIgnore;
  SigIgnore.sa_handler = SIG_IGN;
  sigemptyset(&SigIgnore.sa_mask);
  SigIgnore.sa_flags = 0;

  sigaction(SIGINT,  &SigAction, 0);
  sigaction(SIGTERM, &SigAction, 0);
  sigaction(SIGQUIT, &SigAction, 0);
  sigaction(SIGPIPE, &SigIgnore, 0);              // we want to ignore pipe/fifo read/write errors, we handle them by return codes

  RF.Config_Defaults();
  RF.Config(&Config);

  Filter.Config_Defaults();
  Filter.Config(&Config);
  if(Filter.Enable) Filter.Preset();

  FFT.Config_Defaults();
  FFT.Config(&Config);
  FFT.Preset();

  GSM.Preset();

  HTTP.Config_Defaults();
  if(realpath(ConfigFileName, HTTP.ConfigFileName)==0) HTTP.ConfigFileName[0]=0;
  HTTP.Config(&Config);
  HTTP.Start();

  config_destroy(&Config);

  if(Filter.Enable) Filter.Start();
  FFT.Start();
  GSM.Start();
  RF.Start();

  char Cmd[128];
  while(!RF.StopReq)
  { if(fgets(Cmd, 128, stdin)==0) break;
    UserCommand(Cmd); }

  sleep(4);
  RF.Stop();

  return 0; }


