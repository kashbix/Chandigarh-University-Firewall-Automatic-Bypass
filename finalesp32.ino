#include <WiFi.h>
#include <HTTPClient.h>

// --- 1. CONFIGURATION ---
const char* ssid = ""; //enter your wifi name/ssid
const char* wifi_password = ""; //enter your password of the wifi

const String uid = ""; //enter your uid
const String pass = ""; // enter your password as URL-encoded 

const String gateway_ip = "172.16.2.1";
const int gateway_port = 1000;

String magic_token = "";
String keepalive_url = "";

unsigned long previousMillis = 0;
const long keepalive_interval = 300000; // 5 minutes

// --- FUNCTION PROTOTYPES ---
void sendKeepAlive();
bool loginToFirewall();
bool checkInternet();

// --- 2. HELPER FUNCTION ---
String extractString(String source, String startStr, String endStr) {
  int startIndex = source.indexOf(startStr);
  if (startIndex == -1) return "";
  startIndex += startStr.length();
  int endIndex = source.indexOf(endStr, startIndex);
  if (endIndex == -1) return "";
  return source.substring(startIndex, endIndex);
}

// --- 3. THE TRUTH TEST ---
bool checkInternet() {
  HTTPClient http;
  Serial.println("\n[*] Testing for actual Internet access...");
  http.begin("http://clients3.google.com/generate_204");
  http.setTimeout(5000); 
  int httpCode = http.GET();
  http.end();
  
  if (httpCode == 204) {
    Serial.println("    -> SUCCESS! HTTP 204 received from Google.");
    return true;
  } else {
    Serial.printf("    -> BLOCKED. HTTP Code: %d (Not on the internet yet)\n", httpCode);
    return false;
  }
}

// --- 4. MAIN SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- ESP32 Firewall Authenticator (THE 3-STEP DANCE) ---");
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("Connecting to WiFi network: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, wifi_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[ERROR] FAILED to connect to the physical router!");
    return;
  }
  
  Serial.println("\nWiFi Connected! IP Address: " + WiFi.localIP().toString());

  if (loginToFirewall()) {
    Serial.println("\n[SUCCESS] Authentication sequence completed! You are online.");
  } else {
    Serial.println("\n[FAIL] Authentication sequence failed. Will retry later.");
  }
}

// --- 5. MAIN LOOP ---
void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= keepalive_interval) {
    previousMillis = currentMillis;
    if (WiFi.status() == WL_CONNECTED) {
      if (keepalive_url != "") {
        sendKeepAlive();
      }
    } else {
      Serial.println("WiFi disconnected. Reconnecting...");
      WiFi.reconnect();
    }
  }
}

// --- 6. THE AUTHENTICATION LOGIC ---
// --- 6. THE AUTHENTICATION LOGIC ---
bool loginToFirewall() {
  HTTPClient http;
  String user_agent = "Mozilla/5.0 (X11; Linux x86_64; rv:148.0) Gecko/20100101 Firefox/148.0";
  
  // ==========================================
  // STEP 1: INTERCEPT (Port 80)
  // ==========================================
  Serial.println("\n[1] Intercepting network to spawn token...");
  http.begin("http://neverssl.com/");
  http.setUserAgent(user_agent);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
  
  int httpCode1 = http.GET();
  if (httpCode1 > 0) {
    String html = http.getString();
    
    magic_token = extractString(html, "fgtauth?", "\"");
    if (magic_token == "") magic_token = extractString(html, "name=\"magic\" value=\"", "\"");
    
    if (magic_token != "") {
      magic_token.trim();
      Serial.println("[*] Success! Spawned Magic Token: " + magic_token);
    } else {
      Serial.println("[!] Failed to find magic token in HTML.");
      http.end();
      return false;
    }
  } else {
    Serial.printf("[!] Failed to reach neverssl. Error: %s\n", http.errorToString(httpCode1).c_str());
    http.end();
    return false;
  }
  http.end();

  // ==========================================
  // STEP 2: ACTIVATE (Port 1000)
  // ==========================================
  Serial.println("\n[2] Knocking on Port 1000 to activate session...");
  String auth_page = "http://" + gateway_ip + ":" + String(gateway_port) + "/fgtauth?" + magic_token;
  
  http.begin(auth_page);
  http.setUserAgent(user_agent);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
  
  int httpCode2 = http.GET();
  if (httpCode2 > 0) {
      Serial.println("[*] Session initialized on Port 1000.");
  }
  http.end(); 

  // ==========================================
  // STEP 3: LOGIN (Port 1000)
  // ==========================================
  Serial.println("\n[3] Submitting credentials...");
  
  http.begin("http://" + gateway_ip + ":" + String(gateway_port) + "/");
  http.setUserAgent(user_agent);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  
  // Strict Browser Headers
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Referer", auth_page);
  http.addHeader("Origin", "http://" + gateway_ip + ":" + String(gateway_port));
  
  const char* headerKeys[] = {"Location"};
  http.collectHeaders(headerKeys, 1);
  
  String payload = "4Tredir=http%3A%2F%2Fdetectportal.firefox.com%2Fcanonical.html&magic=" + magic_token + "&username=" + uid + "&password=" + pass;
  
  int httpCode3 = http.POST(payload);
  
  if (httpCode3 > 0) {
    Serial.printf("[*] POST Response Code: %d\n", httpCode3);
    
    // THE NEW FIX: Read the HTML body sent back by the firewall
    String responseBody = http.getString();
    
    // 1. Try checking headers first just in case
    if (httpCode3 == 302 || httpCode3 == 303) {
        String location = http.header("Location");
        if (location.indexOf("keepalive") != -1) {
            if (location.startsWith("/")) keepalive_url = "http://" + gateway_ip + ":" + String(gateway_port) + location;
            else keepalive_url = location;
        }
    }
    
    // 2. THE SCRAPER: Since it's a 200 OK, scrape the HTML for the unique token!
    if (keepalive_url == "") {
        String ka_token = extractString(responseBody, "keepalive?", "\"");
        if (ka_token == "") ka_token = extractString(responseBody, "keepalive?", "'"); // Check for single quotes too
        
        if (ka_token != "") {
            keepalive_url = "http://" + gateway_ip + ":" + String(gateway_port) + "/keepalive?" + ka_token;
        }
    }

    if (keepalive_url != "") {
        Serial.println("[*] Success! Extracted Keep-Alive URL: " + keepalive_url);
    } else {
        Serial.println("[!] Could not scrape keepalive URL. HTML dump snippet:");
        Serial.println(responseBody.substring(0, 200)); // Print a tiny snippet for debugging if it fails
    }

  } else {
    Serial.printf("[!] POST failed. Error: %s\n", http.errorToString(httpCode3).c_str());
  }
  
  http.end();

  // ==========================================
  // FINAL VERIFICATION
  // ==========================================
  if (checkInternet()) {
    Serial.println("\n>>> FIREWALL VAULT CRACKED! You have Internet! <<<");
    if (keepalive_url == "") {
      keepalive_url = "http://" + gateway_ip + ":" + String(gateway_port) + "/keepalive"; // Last resort fallback
    }
    return true;
  }

  return false;
}

// --- 7. THE KEEPALIVE LOGIC ---
void sendKeepAlive() {
  HTTPClient http;
  Serial.println("\n[*] Sending Heartbeat to: " + keepalive_url);
  
  http.begin(keepalive_url);
  http.setUserAgent("Mozilla/5.0 (X11; Linux x86_64; rv:148.0) Gecko/20100101 Firefox/148.0");
  http.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  http.addHeader("Connection", "keep-alive");
  
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    Serial.printf("Connection acknowledged. Code: %d\n", httpCode);
  } else {
    Serial.printf("Connection failed. Code: %d\n", httpCode);
    if (httpCode == -1 || httpCode == 404) {
       Serial.println("Connection lost. Triggering re-authentication...");
       loginToFirewall();
    }
  }
  http.end();
}
