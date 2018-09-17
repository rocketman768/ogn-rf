#include <ogn-rf/rtlsdr.h>

#include <string>
#ifndef __MACH__
#include <sys/sysinfo.h>
#endif /*__MACH__*/

// Forward declarations
class HTTP_Server;
class RF_Acq;

/**
 * \brief Format objects into HTML
 */
class HtmlFormatter {
public:
   /**
    * \param fileDescriptor open unowned filed descriptor to output to
    */
   HtmlFormatter(int fileDescriptor);

   /**
    * \brief Writes a table with class "httpserver"
    */
   void format(HTTP_Server* httpServer) const;
   /**
    * \brief Writes a table with class "rf"
    */
   void format(RF_Acq* rf) const;
   /**
    * \brief Writes a table with class "rtlsdr"
    */
   void format(RTLSDR const& sdr) const;

#ifndef __MACH__
   /**
    * \brief Writes a table with class "sysinfo"
    */
   void format(struct sysinfo const& sysInfo) const;
#endif /*__MACH__*/
  
private:
   int _fd;

   void write(std::string const str) const;
};
