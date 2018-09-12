#include <ogn-rf/htmlformatter.h>

#include <ogn-rf/httpserver.h>
#include <ogn-rf/rfacq.h>
#include <ogn-rf/rtlsdr.h>

#include <stdarg.h>
#include <stdio.h>

#include <string>

static std::string strFmt(const char* fmt, ...) {
   char buf[1024];
   va_list args;
   va_start(args, fmt);
   vsnprintf(buf, sizeof(buf), fmt, args);
   va_end(args);
   return buf;
}

static std::string tableRow(std::string const name, std::string const value) {
   return "<tr><td class=\"name\">" + name + "</td><td class=\"value\">" + value + "</td></tr>";
}

HtmlFormatter::HtmlFormatter(int fileDescriptor) : _fd(fileDescriptor) {}

void HtmlFormatter::format(HTTP_Server* httpServer) const {
   write(tableRow("Host Name", httpServer->Host));
}

void HtmlFormatter::write(std::string const str) const {
   dprintf(_fd, "%s", str.c_str());
}

void HtmlFormatter::format(RF_Acq* rf) const {
}

void HtmlFormatter::format(RTLSDR const& sdr) const {
   if (!sdr.isOpen()) {
      return;
   }

   write(tableRow("RTL-SDR Device", strFmt("%d", sdr.DeviceIndex)));
   write(tableRow("Name", strFmt("%s", sdr.getDeviceName())));
   write(tableRow("Tuner Type", strFmt("%s", sdr.getTunerTypeName())));
   {
      char manuf[256], product[256], serial[256];
      sdr.getUsbStrings(manuf, product, serial);
      write(tableRow("Manufacturer", strFmt("%s", manuf)));
      write(tableRow("Product", strFmt("%s", product)));
      write(tableRow("Serial", strFmt("%s", serial)));
   }
#ifdef NEW_RTLSDR_LIB
   for (int stage = 0; stage < 8; ++stage) {
      char desc[256];
      int gains;
      sdr.getTunerStageGains(stage, 0, desc);
      write(tableRow(strFmt("Tuner Stage #%d", stage), strFmt("%s [%2d]", desc, gains)));
   }
   write(tableRow("Tuner Bandwidths", strFmt("%s", sdr.getTunerBandwidths())));
   write(tableRow("Tuner Gains", strFmt("%d", sdr.getTunerGains())));
#endif /*NEW_RTLSDR_LIB*/
   write(tableRow("Center Frequency", strFmt("%7.3f MHz", sdr.getCenterFreq())));
   write(tableRow("Sample Rate", strFmt("%5.3f MHz", sdr.getSampleRate())));
   //write(tableRow("Frequency Correction", strFmt("%+5.1f ppm", sdr.GSM_FreqCorr)));
   //write(tableRow("Live Time", strFmt("%5.1f%%", sdr.getLiveTime())));
   {
      uint32_t rtlFreq, tunerFreq;
      sdr.getXtalFreq(rtlFreq, tunerFreq);
      write(tableRow("RTL Xtal", strFmt("%8.6f MHz", rtlFreq)));
      write(tableRow("Tuner Xtal", strFmt("%8.6f MHz", tunerFreq)));
   }
}

