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

void HtmlFormatter::write(std::string const str) const {
   dprintf(_fd, "%s", str.c_str());
}

void HtmlFormatter::format(HTTP_Server* httpServer) const {
   write("<table class=\"httpserver\">");
   write(tableRow("Host Name", httpServer->Host));
   write(tableRow("Config File", httpServer->ConfigFileName));
   write("</table>");
}

void HtmlFormatter::format(RF_Acq* rf) const {

   if (!rf) {
      return;
   }

   write("<table class=\"rf\">");
   write(tableRow("Live Time", strFmt("%5.1f%%", 100*rf->getLifeTime())));
   write(tableRow("Total Frequency Correction", strFmt("%5.1f ppm", rf->FreqCorr + rf->GSM_FreqCorr)));
   write(tableRow("RF.FreqPlan", strFmt("%d: %s", rf->HoppingPlan.Plan, rf->HoppingPlan.getPlanName())));
   write(tableRow("RF.Device", strFmt("%d", rf->DeviceIndex)));
   write(tableRow("RF.DeviceSerial", rf->DeviceSerial));
   write(tableRow("RF.SampleRate", strFmt("%3.1f MHz", 1e-6*rf->SampleRate)));
   write(tableRow("RF.FreqCorr", strFmt("%+3d ppm", rf->FreqCorr)));
   write(tableRow("RF.FreqRaster", (rf->FreqRaster ? strFmt("%3d Hz", rf->FreqRaster) : "Off")));
   write(tableRow("RF.BiasTee", (rf->BiasTee > 0 ? "On" : "Off")));
   write(tableRow("RF.OffsetTuning", strFmt("%d", rf->OffsetTuning)));
   write(tableRow("RF.PulseFilter.Threshold", strFmt("%d", rf->PulseFilt.Threshold)));
   write(tableRow("RF.PulseFilter Duty", strFmt("%5.1f ppm", 1e6*rf->PulseFilt.Duty)));
   write(tableRow("RF.OGN.GainMode", strFmt("%d", rf->OGN_GainMode)));
   write(tableRow("RF.OGN.Gain", strFmt("%4.1f dB", 0.1*rf->OGN_Gain)));
   write(tableRow("RF.OGN.StartTime", strFmt("%5.3f s", rf->OGN_StartTime)));
   write(tableRow("RF.OGN.SensTime", strFmt("%5.3f s", static_cast<double>(rf->OGN_SamplesPerRead) / rf->SampleRate)));
   write(tableRow("RF.OGN.SaveRawData", strFmt("%d s", rf->OGN_SaveRawData)));
   write(tableRow("RF.GSM.CenterFreq", strFmt("%5.1f MHz", 1e-6*rf->GSM_CenterFreq)));
   write(tableRow("RF.GSM.FreqCorr", strFmt("%+5.1f ppm", rf->GSM_FreqCorr)));
   write(tableRow("RF.GSM.Scan", strFmt("%d", rf->GSM_Scan)));
   write(tableRow("RF.GSM.Gain", strFmt("%4.1f dB", 0.1*rf->GSM_Gain)));
   write(tableRow("RF.GSM.SensTime", strFmt("%5.3f s", static_cast<double>(rf->GSM_SamplesPerRead) / rf->SampleRate)));
   write("</table>");
}

void HtmlFormatter::format(RTLSDR const& sdr) const {
   if (!sdr.isOpen()) {
      return;
   }

   write("<table class=\"rtlsdr\">");
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
   {
      uint32_t rtlFreq, tunerFreq;
      sdr.getXtalFreq(rtlFreq, tunerFreq);
      write(tableRow("RTL Xtal", strFmt("%8.6f MHz", rtlFreq)));
      write(tableRow("Tuner Xtal", strFmt("%8.6f MHz", tunerFreq)));
   }
   write("</table>");
}

