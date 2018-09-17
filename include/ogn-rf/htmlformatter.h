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
  
private:
   int _fd;

   void write(std::string const str) const;
};
