#include <ogn-rf/gsmfft.h>

#include <ogn-rf/imageutil.h>
#include <ogn-rf/rfacq.h>

#include <algorithm>

   GSM_FFT::GSM_FFT(RF_Acq *RF)
   { Window=0; this->RF=RF; Preset(); }

   int GSM_FFT::Preset(void) { return Preset(RF->SampleRate); }
   int GSM_FFT::Preset(int SampleRate)
   { FFTsize=(8*SampleRate)/15625;
     FFT.PresetForward(FFTsize);
     Window=(Float *)realloc(Window, FFTsize*sizeof(Float));
     FFT.SetSineWindow(Window, FFTsize, (Float)(1.0/sqrt(FFTsize)) );
     PPM_Values.clear(); PPM_Aver=0; PPM_RMS=0; PPM_Points=0; PPM_Time=0;
     return 1; }

   void GSM_FFT::Start(void)
   { StopReq=0; Thr.setExec(ThreadExec); Thr.Create(this); }

   GSM_FFT::~GSM_FFT()
   { Thr.Cancel();
     if(Window) free(Window); }

   double GSM_FFT::getCPU(void) // get CPU time for this thread
   {
#if !defined(__MACH__)
       struct timespec now; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now); return now.tv_sec + 1e-9*now.tv_nsec;
#else
       return 0.0;
#endif
   }

   void *GSM_FFT::ThreadExec(void *Context)
   { GSM_FFT *This = (GSM_FFT *)Context; return This->Exec(); }

   void *GSM_FFT::Exec(void)
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

   void GSM_FFT::Process(void)
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
   void GSM_FFT::AverPeakBkg(Float &Aver, Float &Peak, Float &PeakPos, Float &Bkg, Float *Data, int Size)
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
   void GSM_FFT::AverRMS(Float &Aver, Float &RMS, Float *Data, int Size)
   { Aver=0; RMS=0;
     for(int Idx=0; Idx<Size; Idx++)
     { Aver+=Data[Idx]; }
     Aver/=Size;
     for(int Idx=0; Idx<Size; Idx++)
     { Float Diff=Data[Idx]-Aver; RMS+=Diff*Diff; }
     RMS=sqrt(RMS/Size); }

   int GSM_FFT::ProcessChan(Float &AverPower, int LowBin, int UppBin, Float CenterBin, Float BinWidth, Float CenterFreq)       // process a single GSM channel
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

