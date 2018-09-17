#ifndef RFACQ_H
#define RFACQ_H

#include <ogn-rf/freqplan.h>
#include <ogn-rf/jpeg.h>
#include <ogn-rf/pulsefilter.h>
#include <ogn-rf/rtlsdr.h>

#include <libconfig.h>

class Socket;

class RF_Acq                                    // acquire wideband (1MHz) RF data thus both OGN frequencies at same time
{
public:
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

   static void *ThreadExec(void *Context);

   RF_Acq();
   ~RF_Acq();

   double getLifeTime(void);
   void Config_Defaults(void);
   int config_lookup_float_or_int(config_t *Config, const char *Path, double *Value);
   int Config(config_t *Config);
   int QueueSize(void);
   int Start(void);
   int Stop(void);
   void *Exec(void);
   int calcCenterFreq(uint32_t Time);
} ;

#endif /*RFACQ_H*/
