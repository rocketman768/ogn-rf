#include <ogn-rf/rfacq.h>

#include <ogn-rf/imageutil.h>
#include <ogn-rf/socket.h>

#include <algorithm>

   RF_Acq::RF_Acq() { Config_Defaults();
              GSM_FreqCorr=0;
              // PulseBox.Preset(PulseBoxSize);
              SpectrogramWindow=0;
              StartTime=0; CountAllTimeSlots=0; CountLifeTimeSlots=0;
              StopReq=0; Thr.setExec(ThreadExec); }

  RF_Acq::~RF_Acq() { if(SpectrogramWindow) free(SpectrogramWindow); }

  double RF_Acq::getLifeTime(void)
  { time_t Now; time(&Now); if(Now<=StartTime) return 0;
    return 0.5*CountLifeTimeSlots/(Now-StartTime); }

  void RF_Acq::Config_Defaults(void)
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

  int RF_Acq::config_lookup_float_or_int(config_t *Config, const char *Path, double *Value)
  { int Ret = config_lookup_float(Config, Path, Value); if(Ret==CONFIG_TRUE) return Ret;
    int IntValue; Ret = config_lookup_int(Config, Path, &IntValue); if(Ret==CONFIG_TRUE) { (*Value) = IntValue; return Ret; }
    return Ret; }

  int RF_Acq::Config(config_t *Config)
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

   int RF_Acq::QueueSize(void) { return OutQueue.Size(); }

   int RF_Acq::Start(void) { StopReq=0; return Thr.Create(this); }
   int RF_Acq::Stop(void)  { StopReq=1; return Thr.Join(); }

   void *RF_Acq::ThreadExec(void *Context)
   { RF_Acq *This = (RF_Acq *)Context; return This->Exec(); }

   void *RF_Acq::Exec(void)
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

   int RF_Acq::calcCenterFreq(uint32_t Time)
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
