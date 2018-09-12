#include <ogn-rf/rtlsdr.h>

#include <string>

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

   void format(HTTP_Server* httpServer) const;
   void format(RF_Acq* rf) const;
   void format(RTLSDR const& sdr) const;
  
private:
   int _fd;

   void write(std::string const str) const;
};
