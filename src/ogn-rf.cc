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
#include <ogn-rf/httpserver.h>
#include <ogn-rf/rtlsdr.h>     // SDR radio

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

template <class Float> // scale floating-point data to 8-bit gray scale image
 void LogImage(SampleBuffer<uint8_t> &Image, SampleBuffer<Float> &Data, Float LogRef=0, Float Scale=1, Float Bias=0)
{ Image.Allocate(Data);
  int Pixels=Data.Full;
  for(int Idx=0; Idx<Pixels; Idx++)
  { Float Pixel=Data.Data[Idx];
    if(LogRef)
    { if(Pixel) { Pixel=logf((Float)Pixel/LogRef); Pixel = Pixel*Scale + Bias; }
           else { Pixel=0; } }
    else
    { Pixel = Pixel*Scale + Bias; }
    if(Pixel<0x00) Pixel=0x00;
    else if(Pixel>0xFF) Pixel=0xFF;
    Image.Data[Idx]=(uint8_t)Pixel;
  }
  Image.Full=Pixels;
}

// ==================================================================================================

class RF_Acq                                    // acquire wideband (1MHz) RF data thus both OGN frequencies at same time
{ public:
   int    SampleRate;                           // [Hz] sampling rate

   int    OGN_GainMode;                         // 0=Auto, 1=Manual, 2=Linearity, 3=Sensitivity
   int    OGN_Gain;                             // [0.1dB] Rx gain for OGN reception
   double OGN_StartTime;                        // [sec] when to start acquisition on the center frequency
   int    OGN_SamplesPerRead;                   // [samples] should correspond to about 800 ms of data and be a multiple of 256
                                                // the goal is to listen on center frequency from 0.4 to 1.2 sec
   FreqPlan         HoppingPlan;                // frequency hopping plan (depends on the world region)

   int  DeviceIndex;                            // rtl-sdr device index
   char DeviceSerial[64];                       // serial number of the rtl-sdr device to be selected
   int  OffsetTuning;                           // [bool] this option might be good for E4000 tuner
   int  BiasTee;                                // [bool] T-bias for external LNA power
   int  FreqCorr;                               // [ppm] frequency correction applied to the Rx chip
   int  FreqRaster;                             // [Hz] use only center frequencies on this raster to avoid tuning inaccuracies
   RTLSDR SDR;                                  // SDR receiver (DVB-T stick)
   ReuseObjectQueue< SampleBuffer<uint8_t> > OutQueue; // OGN sample batches are sent there

   Thread Thr;                                  // acquisition thread
   volatile int StopReq;                        // request to stop the acquisition thread

   PulseFilter PulseFilt;

   static const int GSM_GainMode = 1;           // Manual gain mode for GSM
   int GSM_Gain;                                // [0.1dB] Rx gain for GSM frequency calibration
   int GSM_CenterFreq;                          // [Hz] should be selected to cover at lease one broadcast channel in the area
   int GSM_Scan;                                // [bool] scan around the whole GSM band
   int GSM_SamplesPerRead;                      // [samples] should cover one or more frequency correction bursts (100 ms should be enough ?)
   volatile float GSM_FreqCorr;                 // [ppm] frequency correction measured by the GSM frequency calibration
   static const int GSM_LowEdge = 925100000;    // [Hz] E-GSM-900 band, excluding the guards of 100kHz
   static const int GSM_UppEdge = 959900000;    // [Hz]
   static const int GSM_ScanStep =   800000;    // [Hz]
   ReuseObjectQueue< SampleBuffer<uint8_t> > GSM_OutQueue; // GSM sample batches are sent there

   const static uint32_t   OGN_RawDataSync = 0x254F7D01;

   char                    FilePrefix[16];
   int                     OGN_SaveRawData;
   MessageQueue<Socket *>  RawDataQueue;               // sockets send to this queue should be written with a most recent raw data
   MessageQueue<Socket *>  SpectrogramQueue;           // sockets send to this queue should be written with a most recent spectrogram
   DFT1d<float>            SpectrogramFFT;             // FFT to create spectrograms
   int                     SpectrogramFFTsize;
   float                  *SpectrogramWindow;
   SampleBuffer< std::complex<float> > SpectraBuffer;
   SampleBuffer<float>     SpectraPwr;
   SampleBuffer<uint8_t>   Image;
   JPEG                    JpegImage;

   time_t                  StartTime;
   uint32_t                CountAllTimeSlots;
   uint32_t                CountLifeTimeSlots;

  public:
   RF_Acq() { Config_Defaults();
              GSM_FreqCorr=0;
              // PulseBox.Preset(PulseBoxSize);
              SpectrogramWindow=0;
              StartTime=0; CountAllTimeSlots=0; CountLifeTimeSlots=0;
              StopReq=0; Thr.setExec(ThreadExec); }

  ~RF_Acq() { if(SpectrogramWindow) free(SpectrogramWindow); }

  double getLifeTime(void)
  { time_t Now; time(&Now); if(Now<=StartTime) return 0;
    return 0.5*CountLifeTimeSlots/(Now-StartTime); }

  void Config_Defaults(void)
  { SampleRate=1000000;
    OGN_StartTime=0.375; OGN_SamplesPerRead=(850*SampleRate)/1000;
    OGN_GainMode=1; OGN_Gain=600;
    HoppingPlan.setPlan(0);
    PulseFilt.Threshold=0;
    DeviceIndex=0; DeviceSerial[0]=0;
    OffsetTuning=0; FreqCorr=0; FreqRaster=28125; BiasTee=(-1);
    // GSM_CenterFreq=GSM_LowEdge+GSM_ScanStep/2; GSM_Scan=1; GSM_SamplesPerRead=(250*SampleRate)/1000; GSM_Gain=200;
    GSM_CenterFreq=0; GSM_Scan=0; GSM_Gain=200;
    SpectrogramFFTsize=0;
    OGN_SaveRawData=0;
    FilePrefix[0]=0; }

  int config_lookup_float_or_int(config_t *Config, const char *Path, double *Value)
  { int Ret = config_lookup_float(Config, Path, Value); if(Ret==CONFIG_TRUE) return Ret;
    int IntValue; Ret = config_lookup_int(Config, Path, &IntValue); if(Ret==CONFIG_TRUE) { (*Value) = IntValue; return Ret; }
    return Ret; }

  int Config(config_t *Config)
  { const char *Call=0;
    config_lookup_string(Config,"APRS.Call", &Call);
    if(Call) strcpy(FilePrefix, Call);

    double Corr=0.0;
    config_lookup_float_or_int(Config,   "RF.FreqCorr", &Corr); FreqCorr = (int)floor(Corr+0.5);
    config_lookup_int(Config,   "RF.FreqRaster",     &FreqRaster);
    config_lookup_int(Config,   "RF.Device",         &DeviceIndex);
    const char *Serial = 0;
    config_lookup_string(Config,"RF.DeviceSerial",   &Serial);
    if(Serial) { strncpy(DeviceSerial, Serial, 64); DeviceSerial[63]=0; }
    config_lookup_int(Config,   "RF.OfsTune",        &OffsetTuning);
    config_lookup_int(Config,   "RF.BiasTee",        &BiasTee);
    config_lookup_int(Config,   "RF.OGN.GainMode",   &OGN_GainMode);

    config_lookup_int(Config,   "RF.OGN.SaveRawData",   &OGN_SaveRawData);

    SampleRate=1000000;
    if(config_lookup_int(Config, "RF.OGN.SampleRate", &SampleRate)!=CONFIG_TRUE)
    { double Rate;
      if(config_lookup_float(Config, "RF.OGN.SampleRate", &Rate)==CONFIG_TRUE) SampleRate=(int)floor(1e6*Rate+0.5);
      else if(config_lookup_int(Config, "RF.SampleRate", &SampleRate)!=CONFIG_TRUE)
      { if(config_lookup_float(Config, "RF.SampleRate", &Rate)==CONFIG_TRUE) SampleRate=(int)floor(1e6*Rate+0.5); }
    }

    double InpGain= 60.0; config_lookup_float_or_int(Config, "RF.OGN.Gain",         &InpGain); OGN_Gain=(int)floor(InpGain*10+0.5);
           InpGain= 20.0; config_lookup_float_or_int(Config, "RF.GSM.Gain",         &InpGain); GSM_Gain=(int)floor(InpGain*10+0.5);
    double Freq  =     0; config_lookup_float_or_int(Config, "RF.GSM.CenterFreq",   &Freq);    GSM_CenterFreq=(int)floor(Freq*1e6+0.5);
           GSM_Scan =  0; config_lookup_int(Config, "RF.GSM.Scan", &GSM_Scan);

    int PosOK=0;
    int Latitude, Longitude;
    if(config_lookup_int(Config,  "Position.Latitude",   &Latitude)!=CONFIG_TRUE)
    { double Lat;
      if(config_lookup_float(Config,  "Position.Latitude", &Lat)==CONFIG_TRUE)
      { Latitude=(int)floor(Lat*1e7+0.5); }
      else PosOK=(-1); }
    if(config_lookup_int(Config,  "Position.Longitude",  &Longitude)!=CONFIG_TRUE)
    { double Lon;
      if(config_lookup_float(Config,  "Position.Longitude", &Lon)==CONFIG_TRUE)
      { Longitude=(int)floor(Lon*1e7+0.5); }
      else PosOK=(-1); }

    int    Plan=0;
    config_lookup_int(Config, "RF.FreqPlan", &Plan);
    if( (Plan==0) && (PosOK>=0) )
    { Plan=HoppingPlan.calcPlan(Latitude/50*3, Longitude/50*3); }   // decide hopping plan from position
    HoppingPlan.setPlan(Plan);

    PulseFilt.Threshold=0;
    config_lookup_int(Config, "RF.PulseFilter.Threshold",  &PulseFilt.Threshold);

    config_lookup_float(Config, "RF.OGN.StartTime", &OGN_StartTime);
    double SensTime=0.850;
    config_lookup_float(Config, "RF.OGN.SensTime",  &SensTime);
    OGN_SamplesPerRead=(int)floor(SensTime*SampleRate+0.5);
           SensTime=0.250;
    config_lookup_float(Config, "RF.GSM.SensTime",  &SensTime);
    GSM_SamplesPerRead=(int)floor(SensTime*SampleRate+0.5);

    SpectrogramFFTsize=(8*SampleRate)/15625;
    SpectrogramFFT.PresetForward(SpectrogramFFTsize);
    SpectrogramWindow=(float *)realloc(SpectrogramWindow, SpectrogramFFTsize*sizeof(float));
    SpectrogramFFT.SetSineWindow(SpectrogramWindow, SpectrogramFFTsize, (float)(1.0/sqrt(SpectrogramFFTsize)) );

    return 0; }

   int QueueSize(void) { return OutQueue.Size(); }

   int Start(void) { StopReq=0; return Thr.Create(this); }
   int Stop(void)  { StopReq=1; return Thr.Join(); }

   static void *ThreadExec(void *Context)
   { RF_Acq *This = (RF_Acq *)Context; return This->Exec(); }

   void *Exec(void)
   { // printf("RF_Acq.Exec() ... Start\n");
     time(&StartTime); CountAllTimeSlots=0; CountLifeTimeSlots=0;
     char Header[256];
     int Priority = Thr.getMaxPriority(); Thr.setPriority(Priority);
     int CurrCenterFreq = calcCenterFreq(0);
     while(!StopReq)
     { if(SDR.isOpen())                                                    // if device is already open
       { double Now  = SDR.getTime();
         int    IntTimeNow = (int)floor(Now);
         int ReadGSM = (GSM_CenterFreq>0) && ((IntTimeNow%30) == 0); // do the GSM calibration every 30 seconds

         int NextCenterFreq = calcCenterFreq(IntTimeNow+1);          // next center frequency for OGN

         double FracTimeNow = Now-IntTimeNow;
         double WaitTime = OGN_StartTime-FracTimeNow; if(WaitTime<0) WaitTime+=1.0;
         int SamplesToRead=OGN_SamplesPerRead;
         int LifeSlots=2;
         if( ReadGSM || (QueueSize()>1) ) { SamplesToRead/=2; LifeSlots=1; }  // when GSM calibration or data is not being processed fast enough we only read half-time
         if(WaitTime<0.200)
         { usleep((int)floor(1e6*WaitTime+0.5));                              // wait right before the time slot starts
           SampleBuffer<uint8_t> *Buffer = OutQueue.New();                    // get the next buffer to fill with raw I/Q data
           SDR.ResetBuffer();                                                 // needed before every Read()
           int Read=SDR.Read(*Buffer, SamplesToRead);                         // read the time slot raw RF data
           if(Read>0) // RF data Read() successful
           { Buffer->Freq += Buffer->Freq * (1e-6*GSM_FreqCorr);                 // correct the frequency (sign ?)
             if(OGN_SaveRawData>0)
             { time_t Time=(time_t)floor(Buffer->Time);
               struct tm *TM = gmtime(&Time);
               char FileName[32]; sprintf(FileName, "%s_%04d.%02d.%02d.u8", FilePrefix, 1900+TM->tm_year, TM->tm_mon+1, TM->tm_mday);
               FILE *File=fopen(FileName, "ab");
               if(File)
               { Serialize_WriteSync(File, OGN_RawDataSync);
                 Buffer->Serialize(File);
                 fclose(File);
                 OGN_SaveRawData--; }
             }
             PulseFilt.Process(*Buffer);
             if(QueueSize()>1) printf("RF_Acq.Exec() ... Half time slot\n");
             // printf("RF_Acq.Exec() ... SDR.Read() => %d, Time=%16.3f, Freq=%6.1fMHz\n", Read, Buffer->Time, 1e-6*Buffer->Freq);
             while(RawDataQueue.Size())                                       // when a raw data for this slot was requested
             { Socket *Client; RawDataQueue.Pop(Client);
               sprintf(Header, "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nContent-Type: audio/basic\r\n\
Content-Disposition: attachment; filename=\"%s_%07.3fMHz_%03.1fMsps_%14.3fsec.u8\"\r\n\r\n", FilePrefix, 1e-6*Buffer->Freq, 1e-6*Buffer->Rate, Buffer->Time);
               Client->Send(Header);
               Client->Send(Buffer->Data, Buffer->Full);
               Client->SendShutdown(); Client->Close(); delete Client; }
             if(SpectrogramQueue.Size())
             { SlidingFFT(SpectraBuffer, *Buffer, SpectrogramFFT, SpectrogramWindow);
               SpectraPower(SpectraPwr, SpectraBuffer);                                         // calc. spectra power
               float BkgNoise=0.33;
               LogImage(Image, SpectraPwr, (float)BkgNoise, (float)32.0, (float)32.0);  // make the image
               JpegImage.Compress_MONO8(Image.Data, Image.Len, Image.Samples() ); }         // and into JPEG
             while(SpectrogramQueue.Size())
             { Socket *Client; SpectrogramQueue.Pop(Client);
               sprintf(Header, "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nContent-Type: image/jpeg\r\nRefresh: 5\r\n\
Content-Disposition: attachment; filename=\"%s_%07.3fMHz_%03.1fMsps_%10dsec.jpg\"\r\n\r\n",
                        FilePrefix, 1e-6*SpectraBuffer.Freq, 1e-6*SpectraBuffer.Rate*SpectraBuffer.Len/2, (uint32_t)floor(SpectraBuffer.Date+SpectraBuffer.Time));
               Client->Send(Header);
               Client->Send(JpegImage.Data, JpegImage.Size);
               Client->SendShutdown(); Client->Close(); delete Client; }
             if(OutQueue.Size()<4) { OutQueue.Push(Buffer); CountLifeTimeSlots+=LifeSlots; }
                              else { OutQueue.Recycle(Buffer); printf("RF_Acq.Exec() ... Dropped a slot\n"); }
           } else     // RF data Read() failed
           { SDR.Close(); printf("RF_Acq.Exec() ... SDR.Read() failed => SDR.Close()\n"); continue; }
           if(ReadGSM) // if we are to read GSM in the second half-slot
           { SDR.setCenterFreq(GSM_CenterFreq);      // setup for the GSM reception
             SDR.setTunerGainMode(GSM_GainMode);
             SDR.setTunerGain(GSM_Gain);
             GSM_FreqCorr-=(FreqCorr-SDR.getFreqCorrection()); // this is just in case someone changed the frequency correction live
             SDR.setFreqCorrection(FreqCorr);
             SampleBuffer<uint8_t> *Buffer = GSM_OutQueue.New();
             SDR.ResetBuffer();
             int Read=SDR.Read(*Buffer, GSM_SamplesPerRead);
             // printf("RF_Acq.Exec() ...(GSM) SDR.Read() => %d, Time=%16.3f, Freq=%6.1fMHz\n", Read, Buffer->Time, 1e-6*Buffer->Freq);
             if(Read>0)
             { if(GSM_OutQueue.Size()<3) GSM_OutQueue.Push(Buffer);
                                  else { GSM_OutQueue.Recycle(Buffer); printf("RF_Acq.Exec() ... Dropped a GSM batch\n"); }
             }
             SDR.setTunerGainMode(OGN_GainMode);
             SDR.setTunerGain(OGN_Gain);                // back to OGN reception setup
             if(GSM_Scan)
             { GSM_CenterFreq+=GSM_ScanStep;
               if(GSM_CenterFreq>=GSM_UppEdge) GSM_CenterFreq=GSM_LowEdge+GSM_ScanStep/2;
             }
           }
           // if(ReadGSM | OGN_FreqHopChannels)
           { SDR.setCenterFreq(NextCenterFreq); CurrCenterFreq=NextCenterFreq; }
         }
         else usleep(100000);
       }
       else                                                                // if not open yet or was closed due to an error
       { int Index=(-1);
         if(DeviceSerial[0]) Index=SDR.getDeviceIndexBySerial(DeviceSerial);
         if(Index<0) Index=DeviceIndex;
         SDR.FreqRaster = FreqRaster;
         if(SDR.Open(Index, CurrCenterFreq, SampleRate)<0)                    // try to open it
         { printf("RF_Acq.Exec() ... SDR.Open(%d, , ) fails, retry after 1 sec\n", Index); usleep(1000000); }
         else
         { SDR.setOffsetTuning(OffsetTuning);
           if(BiasTee>=0) SDR.setBiasTee(BiasTee);
           SDR.setTunerGainMode(OGN_GainMode);
           SDR.setTunerGain(OGN_Gain);
           SDR.setFreqCorrection(FreqCorr); }
       }
     }

     SDR.Close();
     // printf("RF_Acq.Exec() ... Stop\n");
     return  0; }

   int calcCenterFreq(uint32_t Time)
   { int HopFreq[4];
     HopFreq[0] = HoppingPlan.getFrequency(Time, 0, 0);
     HopFreq[1] = HoppingPlan.getFrequency(Time, 0, 1);
     HopFreq[2] = HoppingPlan.getFrequency(Time, 1, 0);
     HopFreq[3] = HoppingPlan.getFrequency(Time, 1, 1);
     int MidFreq0 = (HopFreq[0]+HopFreq[1]+1)>>1;
     // int MidFreq1 = (HopFreq[2]+HopFreq[3]+1)>>1;
     // int MidFreq  = (MidFreq0+MidFreq1+1)>>1;
     // int FreqDiff =  MidFreq1-MidFreq0;
     int CenterFreq = MidFreq0;
     int Band = SampleRate-150000;
     std::sort(HopFreq, HopFreq+4);
     // if(abs(FreqDiff)<HalfBand) CenterFreq = MidFreq;
     if((HopFreq[3]-HopFreq[0])<Band) CenterFreq=(HopFreq[0]+HopFreq[3]+1)>>1;
     else if((HopFreq[2]-HopFreq[0])<Band) CenterFreq=(HopFreq[0]+HopFreq[2]+1)>>1;
     else if((HopFreq[3]-HopFreq[1])<Band) CenterFreq=(HopFreq[1]+HopFreq[3]+1)>>1;
     // printf("calcCenterFreq(%d): %5.1f-%5.1f-%5.1f-%5.1f [%5.1f] => %5.1f [MHz] %c\n",
     //        Time, 1e-6*HopFreq[0], 1e-6*HopFreq[1], 1e-6*HopFreq[2], 1e-6*HopFreq[3], 1e-6*CenterFreq,
     //              1e-6*(HopFreq[3]-HopFreq[0]), CenterFreq!=MidFreq0?'*':' ');
     return CenterFreq; }

} ;

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

 class GSM_FFT                                      // FFT of the GSM RF data
{ public:

   typedef float Float;
   Thread Thr;                                      // processing thread
   volatile int StopReq;
   RF_Acq *RF;                                      // pointer to the RF acquisition

   int              FFTsize;
   DFT1d<Float>     FFT;
   Float           *Window;

   SampleBuffer< std::complex<Float> > Spectra;     // (complex) spectra
   SampleBuffer< Float >               Power;       // spectra power (energy)

   MessageQueue<Socket *>  SpectrogramQueue;        // sockets send to this queue should be written with a most recent spectrogram
   SampleBuffer<uint8_t>   Image;
   JPEG                    JpegImage;

   std::vector<Float>  PPM_Values;                  // [ppm] measured frequency correction values (a vector of)
   Float               PPM_Aver;                    // [ppm] average frequency correction
   Float               PPM_RMS;                     // [ppm] RMS of the frequency correction
   int                 PPM_Points;                  // number of measurements taken into the average
   time_t              PPM_Time;                    // time when correction measured
   Float            getPPM(void)  const { Float Value=PPM_Aver; return Value; }

  public:
   GSM_FFT(RF_Acq *RF)
   { Window=0; this->RF=RF; Preset(); }

   int Preset(void) { return Preset(RF->SampleRate); }
   int Preset(int SampleRate)
   { FFTsize=(8*SampleRate)/15625;
     FFT.PresetForward(FFTsize);
     Window=(Float *)realloc(Window, FFTsize*sizeof(Float));
     FFT.SetSineWindow(Window, FFTsize, (Float)(1.0/sqrt(FFTsize)) );
     PPM_Values.clear(); PPM_Aver=0; PPM_RMS=0; PPM_Points=0; PPM_Time=0;
     return 1; }

   void Start(void)
   { StopReq=0; Thr.setExec(ThreadExec); Thr.Create(this); }

  ~GSM_FFT()
   { Thr.Cancel();
     if(Window) free(Window); }

   double getCPU(void) // get CPU time for this thread
   {
#if !defined(__MACH__)
       struct timespec now; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now); return now.tv_sec + 1e-9*now.tv_nsec;
#else
       return 0.0;
#endif
   }

   static void *ThreadExec(void *Context)
   { GSM_FFT *This = (GSM_FFT *)Context; return This->Exec(); }

   void *Exec(void)
   { // printf("GSM_FFT.Exec() ... Start\n");
     while(!StopReq)
     { double ExecTime=getCPU();
       SampleBuffer<uint8_t> *InpBuffer = RF->GSM_OutQueue.Pop();                         // get data sample on a GSM frequency
       // printf("GSM_FFT.Exec() ... (%5.3fMHz, %5.3fsec, %dsamples)\n", 1e-6*InpBuffer->Freq, InpBuffer->Time, InpBuffer->Full/2);
       SlidingFFT(Spectra, *InpBuffer, FFT, Window);                                      // perform sliding-FFT on the data
       SpectraPower(Power, Spectra);                                                      // calculate power of the spectra
       RF->GSM_OutQueue.Recycle(InpBuffer);                                               // return the buffer to the queue for reuse
       if(SpectrogramQueue.Size())                                                        // of spectrogram was requested
       { LogImage(Image, Power, (Float)0.33, (Float)32.0, (Float)32.0);                   // create spectrogram image
         JpegImage.Compress_MONO8(Image.Data, Image.Len, Image.Samples() ); }
       while(SpectrogramQueue.Size())                                                     // send the image to all requesters
       { Socket *Client; SpectrogramQueue.Pop(Client);
         Client->Send("HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nContent-Type: image/jpeg\r\nRefresh: 10\r\n\r\n");
         // printf("GSM_FFT.Exec() ... Request for (GSM)spectrogram\n");
         Client->Send(JpegImage.Data, JpegImage.Size);
         Client->SendShutdown(); Client->Close(); delete Client; }
       Process();                                                                         // process the data to find frequency calibration markers
       ExecTime=getCPU()-ExecTime; // printf("GSM_FFT.Exec() ... %5.3fsec\n", ExecTime);
     }
     // printf("GSM_FFT.Exec() ... Stop\n");
     return 0; }

   static const int ChanWidth = 200000; // [Hz] GSM channel width
   static const int DataRate  = 270833; // [Hz] GSM data rate
   SampleBuffer<Float> Aver, Peak, PeakPos, Bkg;

   void Process(void)
   { Float BinWidth=Power.Rate/2;                                           // [Hz] FFT bin spectral width
     int Bins = Power.Len;                                                  // [int] number of FFT bins
     Float FirstBinFreq = Power.Freq-BinWidth*Bins/2;                       // [Hz] center frequency of the first FFT bin
     Float LastBinFreq  = Power.Freq+BinWidth*Bins/2;                       // [Hz] center frequency of the one-after-the-last FFT bin

     int Chan = (int)ceil(FirstBinFreq/ChanWidth);                          // integer channel number corr. to the first FFT bin (GSM channels are on multiples of 200kHz)
     for( ; ; Chan++)                                                       // loop over (possible) channels in this scan
     { Float CenterFreq=Chan*ChanWidth; if(CenterFreq>=LastBinFreq) break;  // center frequency of the channel
       Float LowFreq = CenterFreq-0.45*ChanWidth;                           // [Hz] lower frequency to measure the channel
       Float UppFreq = CenterFreq+0.45*ChanWidth;                           // [Hz] upper frequency to measure the channel
       int LowBin=(int)floor((LowFreq-FirstBinFreq)/BinWidth+0.5);          // FFT bins corresponding to the channel frequency range
       int UppBin=(int)floor((UppFreq-FirstBinFreq)/BinWidth+0.5);
       if( (LowBin<0) || (LowBin>=Bins) ) continue;                         // skip this channel if range to measure
       if( (UppBin<0) || (UppBin>=Bins) ) continue;                         // not contained completely in this scan
       Float AverPower;
       int Marks=ProcessChan(AverPower, LowBin, UppBin, (CenterFreq-FirstBinFreq)/BinWidth, BinWidth, CenterFreq);
       if(Marks==1) PPM_Values.pop_back(); // if only one mark found, drop it - likely a false signal
       printf("GSM_FFT::Process: Chan=%d, Freq=%8.3fMHz [%4d-%4d] %+6.1fdB %d marks\n", Chan, 1e-6*CenterFreq, LowBin, UppBin, 10*log10(AverPower), Marks);
       // { char FileName[32]; sprintf(FileName, "GSM_%5.1fMHz.dat", 1e-6*CenterFreq);
       //   FILE *File=fopen(FileName, "wt");
       //   for(int Idx=0; Idx<Aver.Full; Idx++)
       //  { fprintf(File, "%5d %12.6f %12.6f %+10.6f %10.6f\n",
       //                   Idx, Aver[Idx], Peak[Idx], PeakPos[Idx], Bkg[Idx]); }
       //   fclose(File); }
     }

     std::sort(PPM_Values.begin(), PPM_Values.end());

     if(PPM_Values.size()>=16)                                         // if at least 16 measured points
     { Float Aver, RMS; int Margin=PPM_Values.size()/4;
       AverRMS(Aver, RMS, PPM_Values.data()+Margin, PPM_Values.size()-2*Margin);
       // printf("PPM = %+7.3f (%5.3f) [%d]\n", Aver, RMS, PPM_Values.size()-2*Margin);
       if(RMS<0.5)
       { PPM_Aver=Aver; PPM_RMS=RMS; PPM_Points=PPM_Values.size()-2*Margin; PPM_Time=(time_t)floor(Power.Time+0.5); PPM_Values.clear();
         printf("GSM freq. calib. = %+7.3f +/- %5.3f ppm, %d points\n", PPM_Aver, PPM_RMS, PPM_Points);
         Float Corr=RF->GSM_FreqCorr; Corr+=0.25*(PPM_Aver-Corr); RF->GSM_FreqCorr=Corr; }
       PPM_Values.clear();
     }

     if(PPM_Values.size()>=8)                                          // if at least 8 measured points
     { Float Aver, RMS;
       AverRMS(Aver, RMS, PPM_Values.data()+1, PPM_Values.size()-2);   // calc. the average excluding two extreme points
       // printf("PPM = %+7.3f (%5.3f) [%d]\n", Aver, RMS, PPM_Values.size()-2);
       if(RMS<0.5)
       { PPM_Aver=Aver; PPM_RMS=RMS; PPM_Points=PPM_Values.size()-2; PPM_Time=(time_t)floor(Power.Time+0.5); PPM_Values.clear();
         printf("GSM freq. calib. = %+7.3f +/- %5.3f ppm, %d points\n", PPM_Aver, PPM_RMS, PPM_Points);
         Float Corr=RF->GSM_FreqCorr; Corr+=0.25*(PPM_Aver-Corr); RF->GSM_FreqCorr=Corr; }
     }

   }

   // Average, Peak (with Position) and Background = Average - values around the Peak
   static void AverPeakBkg(Float &Aver, Float &Peak, Float &PeakPos, Float &Bkg, Float *Data, int Size)
   { Aver=0; Peak=0; PeakPos=0; int PeakIdx=0;
     for(int Idx=0; Idx<Size; Idx++)
     { Float Dat=Data[Idx];
       if(Dat>Peak) { Peak=Dat; PeakIdx=Idx; }
       Aver+=Dat; }
     if(PeakIdx==0)             { Peak+=Data[     1];                    PeakPos=PeakIdx+Data[     1]/Peak;                      Bkg=(Aver-Peak)/(Size-2); }
     else if(PeakPos==(Size-1)) { Peak+=Data[Size-2];                    PeakPos=PeakIdx-Data[Size-2]/Peak;                      Bkg=(Aver-Peak)/(Size-2); }
     else                       { Peak+=Data[PeakIdx+1]+Data[PeakIdx-1]; PeakPos=PeakIdx+(Data[PeakIdx+1]-Data[PeakIdx-1])/Peak; Bkg=(Aver-Peak)/(Size-3); }
     Aver/=Size; }

   // average and RMS of a data series
   static void AverRMS(Float &Aver, Float &RMS, Float *Data, int Size)
   { Aver=0; RMS=0;
     for(int Idx=0; Idx<Size; Idx++)
     { Aver+=Data[Idx]; }
     Aver/=Size;
     for(int Idx=0; Idx<Size; Idx++)
     { Float Diff=Data[Idx]-Aver; RMS+=Diff*Diff; }
     RMS=sqrt(RMS/Size); }

   int ProcessChan(Float &AverPower, int LowBin, int UppBin, Float CenterBin, Float BinWidth, Float CenterFreq)       // process a single GSM channel
   { int Slides = Power.Samples();                                                                                    // [FFT slides] in the data
     int Bins = Power.Len;                                                                                            // number of FFT bins
     Aver.Allocate(1, Slides); Peak.Allocate(1, Slides); PeakPos.Allocate(1, Slides); Bkg.Allocate(1, Slides);
     Float *Data = Power.Data;
     for(int Idx=0; Idx<Slides; Idx++, Data+=Bins)
     { AverPeakBkg(Aver[Idx], Peak[Idx], PeakPos[Idx], Bkg[Idx], Data+LowBin, UppBin-LowBin+1);
       PeakPos[Idx]+=LowBin-CenterBin; }
     Aver.Full=Slides; Peak.Full=Slides; PeakPos.Full=Slides; Bkg.Full=Slides;
     Float PowerRMS; AverRMS(AverPower, PowerRMS, Aver.Data, Slides);
     // printf("AverPower=%3.1f, PowerRMS=%3.1f\n", AverPower, PowerRMS);
     if(PowerRMS>(0.5*AverPower)) return 0;          // skip pulsing channels

     Float AverPeak, PeakRMS; AverRMS(AverPeak, PeakRMS, Peak.Data, Slides);
     Float AverBkg, BkgRMS; AverRMS(AverBkg, BkgRMS, Bkg.Data, Slides);
     // printf("AverPeak=%3.1f, PeakRMS=%3.1f, AverBkg=%5.3f, BkgRMS=%5.3f\n", AverPeak, PeakRMS, AverBkg, BkgRMS);

     int Marks=0;
     Float PeakThres = 4*PeakRMS;
     Float BkgThres  = 4*BkgRMS;
     for(int Idx=1; Idx<(Slides-1); Idx++)
     { Float PeakL=Peak.Data[Idx-1]-AverPeak;
       Float PeakM=Peak.Data[Idx  ]-AverPeak;
       Float PeakR=Peak.Data[Idx+1]-AverPeak;
       Float PeakSum = PeakL+PeakM+PeakR;
       if(PeakSum<=PeakThres) continue;
       if(PeakM<PeakL)  continue;
       if(PeakM<=PeakR) continue;
       if(PeakM<=((PeakL+PeakR)/2)) continue;
       Float BkgSum = Bkg.Data[Idx-1]+Bkg.Data[Idx]+Bkg.Data[Idx+1];
       if((3*AverBkg-BkgSum)<BkgThres) continue;
       if(Peak.Data[Idx]<(40*Bkg.Data[Idx])) continue;
       Float PPM = -1e6*(PeakPos.Data[Idx]*BinWidth-(Float)DataRate/4)/CenterFreq;
       // printf("Mark: PeakSum[%5d]=%8.1f/%6.1f Bkg=%8.3f/%6.3f Peak/Bkg=%8.1f PeakPos=%+7.3f %+7.3fppm\n",
       //         Idx, PeakSum, PeakThres, 3*AverBkg-BkgSum, BkgThres, Peak.Data[Idx]/Bkg.Data[Idx], PeakPos.Data[Idx], PPM);
       PPM_Values.push_back(PPM);
       Marks++; }

     return Marks; }

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

