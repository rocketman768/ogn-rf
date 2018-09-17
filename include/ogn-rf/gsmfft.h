#ifndef GSMFFT_H
#define GSMFFT_H

#include <ogn-rf/buffer.h>
#include <ogn-rf/jpeg.h>
#include <ogn-rf/socket.h>
#include <ogn-rf/thread.h>

#include <vector>

class RF_Acq;

class GSM_FFT                                      // FFT of the GSM RF data
{
public:

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

   static const int ChanWidth = 200000; // [Hz] GSM channel width
   static const int DataRate  = 270833; // [Hz] GSM data rate
   SampleBuffer<Float> Aver, Peak, PeakPos, Bkg;


   GSM_FFT(RF_Acq *RF);
   ~GSM_FFT();

   int Preset(void);
   int Preset(int SampleRate);
   Float getPPM(void) const;

   void Start(void);

   // get CPU time for this thread
   double getCPU(void);

   // process a single GSM channel
   int ProcessChan(Float &AverPower, int LowBin, int UppBin, Float CenterBin, Float BinWidth, Float CenterFreq);

   void *Exec(void);
   void Process(void);

   static void *ThreadExec(void *Context);
   // Average, Peak (with Position) and Background = Average - values around the Peak
   static void AverPeakBkg(Float &Aver, Float &Peak, Float &PeakPos, Float &Bkg, Float *Data, int Size);
   // average and RMS of a data series
   static void AverRMS(Float &Aver, Float &RMS, Float *Data, int Size);
};

#endif /*GSMFFT_H*/

