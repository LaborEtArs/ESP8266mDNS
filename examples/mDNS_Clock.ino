/*
  ESP8266 mDNS responder clock

  This is an example of the dynamic MDNS service TXT feature.
  A 'clock' service in announced via the MDNS responder and the current
  time is set as a TXT item (eg. 'curtime=Mon Oct 15 19:54:35 2018').
  The time value is updated every minute.
  
  The ESP is initially announced to clients as 'esp8266.local', if this host domain
  is already used in the local network, another host domain is negociated. Keep an
  eye to the serial output to learn the final host domain for the clock service.
  The service itself is is announced as 'host domain'._espclk._tcp.local.
  As the service uses port 80, a very simple HTTP server is installed also to deliver
  a small web page containing a greeting and the current time (not updated).
  The web server code is taken nearly 1:1 from the 'mDNS_Web_Server.ino' example.
  Point your browser to 'host domain'.local to see this web page.

  Instructions:
  - Update WiFi SSID and password as necessary.
  - Flash the sketch to the ESP8266 board
  - Install host software:
    - For Linux, install Avahi (http://avahi.org/).
    - For Windows, install Bonjour (http://www.apple.com/support/bonjour/).
    - For Mac OSX and iOS support is built in through Bonjour already.
  - Use a MDNS/Bonjour browser like 'Discovery' to find the clock service in your local
    network and see the current time updates.

*/


#include <ESP8266WiFi.h>
#include "../ESP8266mDNS.h"
#include <WiFiClient.h>
#include <time.h>

const char*                 ssid					= "............";
const char*                 password				= "............";

char*                       pcHostDomain			= 0;       	// Negociated host domain
bool                        bHostDomainConfirmed	= false;	// Flags the confirmation of the host domain
MDNSResponder::hMDNSService hMDNSService			= 0;       	// The handle of the clock service in the MDNS responder

#define TIMEZONE_OFFSET     1                               	// CET
#define DST_OFFSET          1                               	// CEST

#define SERVICE_PORT        80                              	// HTTP port

// TCP server at port 'SERVICE_PORT' will respond to HTTP requests
WiFiServer server(SERVICE_PORT);

/*
 * getTimeString
 */
const char* getTimeString(void) {

  static char	acTimeString[32];
  time_t now = time(nullptr);
  ctime_r(&now, acTimeString);
  acTimeString[os_strlen(acTimeString) - 1] = 0;	// Remove trailing line break...
  return acTimeString;
}


/*
 * setClock
 *
 * Set time via NTP
 */
void setClock(void) {
  configTime((TIMEZONE_OFFSET * 3600), (DST_OFFSET * 3600), "pool.ntp.org", "time.nist.gov", "time.windows.com");

  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  Serial.printf("Current time: %s\n", getTimeString());
}


/*
 * strrstr
 *
 * Search in s1 for s2 from behind.
 * From: https://stackoverflow.com/a/1634398/2778898
 */
static char* strrstr(char *__restrict s1, const char *__restrict s2) {
  size_t    s1len = strlen(s1);
  size_t    s2len = strlen(s2);
  char*     s;

  if (s2len > s1len) {
    return NULL;
  }
  for (s = s1 + s1len - s2len; s >= s1; --s) {
    if (0 == strncmp(s, s2, s2len)) {
      return s;
    }
  }
  return NULL;
}


/*
 * updateDomain
 *
 * Updates the given domain 'p_rpcHostname' by appending a delimiter and an index number.
 *
 * If the given domain already hasa numeric index (after the given delimiter), this index
 * incremented. If not, the delimiter and index '2' is added.
 *
 * If 'p_rpcHostname' is empty (==0), the given default name 'p_pcDefaultHostname' is used,
 * if no default is given, 'esp8266' is used.
 */
bool updateDomain(char*& p_rpcHostname,
                  const char* p_pcDivider /*= "-"*/,
                  const char* p_pcDefaultHostname /*= 0*/) {

  bool	bResult = false;
  
  // Ensure a divider exists; use '-' as default
  const char*   pcDivider = (p_pcDivider ?: "-");
  
  if (p_rpcHostname) {
    char*   pFoundDivider = strrstr(p_rpcHostname, pcDivider);
    if (pFoundDivider) {    // maybe already extended
      char*         pEnd = 0;
      unsigned long ulIndex = strtoul((pFoundDivider + os_strlen(pcDivider)), &pEnd, 10);
      if ((ulIndex) &&
          ((pEnd - p_rpcHostname) == os_strlen(p_rpcHostname)) &&
          (!*pEnd)) {       // Valid (old) index found
        
        char    acIndexBuffer[16];
        sprintf(acIndexBuffer, "%lu", (++ulIndex));
        size_t  stLength = ((pFoundDivider - p_rpcHostname + os_strlen(pcDivider)) + os_strlen(acIndexBuffer) + 1);
        char*   pNewHostname = new char[stLength];
        if (pNewHostname) {
          memcpy(pNewHostname, p_rpcHostname, (pFoundDivider - p_rpcHostname + os_strlen(pcDivider)));
          pNewHostname[pFoundDivider - p_rpcHostname + os_strlen(pcDivider)] = 0;
          os_strcat(pNewHostname, acIndexBuffer);
          
          delete[] p_rpcHostname;
          p_rpcHostname = pNewHostname;
          
          bResult = true;
        }
        else {
          Serial.println("updateDomain: FAILED to alloc new hostname!");
        }
      }
      else {
        pFoundDivider = 0;  // Flag the need to (base) extend the hostname
      }
    }
    
    if (!pFoundDivider) {   // not yet extended (or failed to increment extension) -> start indexing
      size_t    stLength = os_strlen(p_rpcHostname) + (os_strlen(pcDivider) + 1 + 1);	// Name + Divider + '2' + '\0'
      char*     pNewHostname = new char[stLength];
      if (pNewHostname) {
        sprintf(pNewHostname, "%s%s2", p_rpcHostname, pcDivider);
        
        delete[] p_rpcHostname;
        p_rpcHostname = pNewHostname;
        
        bResult = true;
      }
      else {
        Serial.println("updateDomain: FAILED to alloc new hostname!");
      }
    }
  }
  else {
      // No given host domain, use base or default
    const char* cpcDefaultName = (p_pcDefaultHostname ?: "esp8266");
    
    size_t      stLength = os_strlen(cpcDefaultName) + 1;	// '\0'
    p_rpcHostname = new char[stLength];
    if (p_rpcHostname) {
      os_strncpy(p_rpcHostname, cpcDefaultName, stLength);
      bResult = true;
    }
    else {
      Serial.println("updateDomain: FAILED to alloc new hostname!");
    }
  }
  Serial.printf("updateDomain: %s\n", p_rpcHostname);
  return bResult;
}


/*
 * setStationHostname
 */
bool setStationHostname(const char* p_pcHostname) {

  if (p_pcHostname) {
    WiFi.hostname(p_pcHostname);
    Serial.printf("setDeviceHostname: Station hostname is set to '%s'\n", p_pcHostname);
  }
  return true;
}


/*
 * MDNSDynamicServiceTxtCallback
 *
 * Add a dynamic MDNS TXT item 'ct' to the clock service.
 * The callback function is called every time, the TXT items for the clock service
 * are needed.
 * This can be triggered by calling MDNS.announce().
 *
 */
bool MDNSDynamicServiceTxtCallback(MDNSResponder* p_pMDNSResponder,
                                   const MDNSResponder::hMDNSService p_hService,
                                   void* p_pUserdata) {
  (void) p_pUserdata;
  
  if ((p_pMDNSResponder) &&
      (hMDNSService == p_hService)) {
    Serial.printf("Updating curtime TXT item to: %s\n", getTimeString());
    p_pMDNSResponder->addDynamicServiceTxt(p_hService, "curtime", getTimeString());
  }
  return true;
}


/*
 * MDNSProbeResultCallback
 *
 * Probe result callback for the host domain.
 * If the domain is free, the host domain is set and the clock service is
 * added.
 * If the domain is already used, a new name is created and the probing is
 * restarted via p_pMDNSResponder->setHostname().
 *
 */
bool MDNSProbeResultCallback(MDNSResponder* p_pMDNSResponder,
                             const char* p_pcDomainName,
                             const MDNSResponder::hMDNSService p_hService,
                             bool p_bProbeResult,
                             void* p_pUserdata) {
  (void) p_pUserdata;
  
  if ((p_pMDNSResponder) &&
      (0 == p_hService)) {  // Called for host domain
    Serial.printf("MDNSProbeResultCallback: Host domain '%s.local' is %s\n", p_pcDomainName, (p_bProbeResult ? "free" : "already USED!"));
    if (true == p_bProbeResult) {
      // Set station hostname
      setStationHostname(pcHostDomain);

      if (!bHostDomainConfirmed) {
        // Hostname free -> setup clock service
        bHostDomainConfirmed = true;

        if (!hMDNSService) {
          // Add a 'clock.tcp' service to port 'SERVICE_PORT', using the host domain as instance domain
          hMDNSService = p_pMDNSResponder->addService(0, "espclk", "tcp", SERVICE_PORT);
          if (hMDNSService) {
            // Add a simple static MDNS service TXT item
            p_pMDNSResponder->addServiceTxt(hMDNSService, "port#", SERVICE_PORT);
            // Set the callback function for dynamic service TXTs
            p_pMDNSResponder->setDynamicServiceTxtCallback(hMDNSService, MDNSDynamicServiceTxtCallback, 0);
          }
        }
      }
      else {
        // Change hostname, use '-' as divider between base name and index
        if (updateDomain(pcHostDomain, "-", 0)) {
          p_pMDNSResponder->setHostname(pcHostDomain);
        }
        else {
          Serial.println("MDNSProbeResultCallback: FAILED to update hostname!");
        }
      }
    }
  }
  return true;
}


/*
 * handleHTTPClient
 */
void handleHTTPClient(WiFiClient& client) {
  Serial.println("");
  Serial.println("New client");

  // Wait for data from client to become available
  while (client.connected() && !client.available()) {
    delay(1);
  }

  // Read the first line of HTTP request
  String req = client.readStringUntil('\r');

  // First line of HTTP request looks like "GET /path HTTP/1.1"
  // Retrieve the "/path" part by finding the spaces
  int addr_start = req.indexOf(' ');
  int addr_end = req.indexOf(' ', addr_start + 1);
  if (addr_start == -1 || addr_end == -1) {
    Serial.print("Invalid request: ");
    Serial.println(req);
    return;
  }
  req = req.substring(addr_start + 1, addr_end);
  Serial.print("Request: ");
  Serial.println(req);
  client.flush();

  // Get current time
  time_t now = time(nullptr);;
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
 
  String s;
  if (req == "/") {
    IPAddress ip = WiFi.localIP();
    String ipStr = String(ip[0]) + '.' + String(ip[1]) + '.' + String(ip[2]) + '.' + String(ip[3]);
    s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>Hello from ESP8266 at ";
    s += ipStr;
    // Simple addition of the current time
    s += "\r\nCurrent time is: ";
    s += getTimeString();
    // done :-)
    s += "</html>\r\n\r\n";
    Serial.println("Sending 200");
  } else {
    s = "HTTP/1.1 404 Not Found\r\n\r\n";
    Serial.println("Sending 404");
  }
  client.print(s);

  Serial.println("Done with client");
}


/*
 * setup
 */
void setup(void) {
  Serial.begin(115200);
  
  // Connect to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Sync clock
  setClock();
  
  // Setup MDNS responder
  MDNS.setProbeResultCallback(MDNSProbeResultCallback, 0);
  // Init the (currently empty) host domain string with 'esp8266'
  if ((!updateDomain(pcHostDomain, 0, "esp8266")) ||
      (!MDNS.begin(pcHostDomain))) {
    Serial.println("Error setting up MDNS responder!");
    while (1) { // STOP
      delay(1000);
    }
  }
  Serial.println("MDNS responder started");

  // Start TCP (HTTP) server
  server.begin();
  Serial.println("TCP server started");
}


/*
 * loop
 */
void loop(void) {
  // Check if a client has connected
  WiFiClient    client = server.available();
  if (client) {
    handleHTTPClient(client);
  }
  
  // Allow MDNS processing
  MDNS.update();
  
  // Update time (if needed)
  static    unsigned long ulNextTimeUpdate = (60 * 1000);
  if (ulNextTimeUpdate < millis()) {
      
      if (hMDNSService) {
          // Just trigger a new MDNS announcement, this will lead to a call to
          // 'MDNSDynamicServiceTxtCallback', which will update the time TXT item
          MDNS.announce();
      }
      ulNextTimeUpdate = (millis() + (60 * 1000));
  }
}


