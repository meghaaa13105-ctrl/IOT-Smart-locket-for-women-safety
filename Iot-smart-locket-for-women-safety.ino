#include <Wire.h>  // Needed for MAX30105
// #include "MPU9250.h" // COMMENTED OUT (kept for reference)
// #include <MPU6050.h> // also commented out if needed later

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "MAX30105.h"
#include "heartRate.h"
// Note: you already had <base64.h> available earlier. To keep compatibility with the working code
// below we include our own base64Encode() helper (from your working MPU example).
// If you prefer the base64::encode from the library, you may swap it in.

// WiFi credentials
#define WIFI_SSID "pts"
#define WIFI_PASSWORD "1029384756"

// Gmail SMTP settings
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "meghhaa133@gmail.com"
#define AUTHOR_PASSWORD "vueguearosjfwxbk"  // <--- IMPORTANT: Use a Gmail App Password!
#define RECIPIENT_EMAIL "prakruthi873@gmail.com"
#define RECIPIENT_NAME "Emergency Contact"

// Sensor pins
#define MAX4466_PIN 34
#define BUTTON_PIN 18

// Thresholds
#define VOICE_THRESHOLD 2600 // Adjust based on your microphone and environment
#define VOICE_DURATION  50    // Time in ms to confirm voice activity
#define HEART_RATE_MIN 50     // Minimum safe heart rate (BPM)
#define HEART_RATE_MAX 120    // Maximum safe heart rate (BPM)
// Fall detection related thresholds REMOVED
#define COOLDOWN_PERIOD 10000  // 30 seconds cooldown between alerts

// Objects
// MPU9250 imu; // COMMENTED OUT (reserved)
MAX30105 heartSensor;
WiFiClientSecure client;

// Variables
unsigned long lastAlertTime = 0;
bool alertInProgress = false;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;

// Gmail async retry controls (small helper, from your working MPU sketch)
bool gmailPending = false;
unsigned long gmailStart = 0;
String gmailSubject = "";
String gmailBody = "";
const int MAX_RETRIES = 3;
int gmailRetryCount = 0;

// Forward declarations
void connectWiFi();
bool checkVoiceActivation();
int readHeartRate();
void triggerSOS(String reason);
bool sendGmail(String subject, String body);
String createHTMLEmail(String reason);
String getTimestamp();

// Base64 helper (from your working MPU sketch)
const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
String base64Encode(String input) {
  String output = "";
  int val = 0, valb = -6;
  for (int i = 0; i < input.length(); i++) {
    val = (val << 8) + (uint8_t)input[i];
    valb += 8;
    while (valb >= 0) {
      output += b64chars[(val >> valb) & 0x3F];
      valb -= 6;
    }
  }
  if (valb > -6) output += b64chars[((val << 8) >> (valb + 8)) & 0x3F];
  while (output.length() % 4) output += '=';
  return output;
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n\nWomen Safety System Starting...");

  pinMode(MAX4466_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);  // I2C on ESP32 default pins SDA=21, SCL=22

  // --- MPU initialization (commented out) ---
  // Serial.println("Initializing MPU (commented out)...");
  // imuStatus = imu.begin();
  // if (imuStatus < 0) { ... }
  // (kept commented for later use)

  // --- Initialize MAX30102 ---
  Serial.println("Initializing MAX30102...");
  if (!heartSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("✗ MAX30102 not found");
  } else {
    Serial.println("✓ MAX30102 connected");
    heartSensor.setup();                     // default configuration
    heartSensor.setPulseAmplitudeRed(0x0A);  // red LED
    heartSensor.setPulseAmplitudeGreen(0);   // green off if not used
  }

  // --- Connect to WiFi ---
  connectWiFi();

  Serial.println("\n=== Women Safety System Active ===");
  Serial.print("Active sensors: Voice");
  Serial.println(" + Heart Rate\n");
}

void loop() {
  // Manual button SOS
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Manual SOS Triggered!");
      triggerSOS("Manual Button Press");
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);  // wait release
    }
  }

  // Voice activation
  if (checkVoiceActivation()) {
    Serial.println("Voice SOS Triggered!");
    triggerSOS("Voice Distress Signal");
  }

  // Heart rate monitoring
  int heartRate = readHeartRate();
  if (heartRate > 0) {
    if (heartRate < HEART_RATE_MIN) {
      Serial.println("Low Heart Rate Detected!");
      triggerSOS("Low Heart Rate: " + String(heartRate) + " BPM");
    } else if (heartRate > HEART_RATE_MAX) {
      Serial.println("High Heart Rate Detected!");
      triggerSOS("High Heart Rate: " + String(heartRate) + " BPM");
    }
  }

  // If a gmail send is queued (non-blocking pattern) attempt to send in background
  if (gmailPending) {
    bool sent = sendGmail(gmailSubject, gmailBody);
    if (sent) {
      gmailPending = false;
      gmailRetryCount = 0;
      Serial.println("📧 Gmail successfully sent!");
    } else {
      gmailRetryCount++;
      Serial.println("⚠ Gmail send failed, retry " + String(gmailRetryCount));
      if (gmailRetryCount >= MAX_RETRIES) {
        gmailPending = false;
        gmailRetryCount = 0;
        Serial.println("❌ Max retries reached, giving up.");
      }
    }
  }

  delay(100);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n✗ WiFi Failed to connect!");
  }
}

bool checkVoiceActivation() {
  int voiceLevel = analogRead(MAX4466_PIN);  // read mic amplifier output
  if (voiceLevel > VOICE_THRESHOLD) {
    Serial.print("Voice detected (level): ");
    Serial.println(voiceLevel);
    delay(VOICE_DURATION);
    int confirmLevel = analogRead(MAX4466_PIN);
    return (confirmLevel > VOICE_THRESHOLD);
  }
  return false;
}

int readHeartRate() {
  long irValue = heartSensor.getIR();

  if (irValue < 50000) return 0;  // finger not placed or IR too low

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;

      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
    if(beatAvg > 0){
      Serial.print("Heart Rate:  ");
      Serial.print(beatAvg);
      Serial.println(" bpm ");
;
    }
  }
  return 0;
}

void triggerSOS(String reason) {
  unsigned long currentTime = millis();

  if (alertInProgress || (currentTime - lastAlertTime < COOLDOWN_PERIOD)) {
    Serial.println("Alert cooldown active or alert already in progress. Skipping new alert.");
    return;
  }

  alertInProgress = true;
  lastAlertTime = currentTime;

  Serial.println("\n========================================");
  Serial.println("🚨 SOS ALERT INITIATED!");
  Serial.println("Reason: " + reason);
  Serial.println("========================================\n");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting to reconnect for email...");
    connectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Queue email for background send (non-blocking to the main sensors)
    gmailSubject = "🚨 EMERGENCY SOS ALERT - Women Safety Device";
    gmailBody = createHTMLEmail(reason);  // HTML body
    gmailPending = true;
    gmailStart = millis();
    Serial.println("Email queued for send.");
  } else {
    Serial.println("✗ No WiFi connection available. Email not queued.");
  }

  alertInProgress = false;
}

// --------- sendGmail function (uses the working MPU approach) ----------
bool sendGmail(String subject, String body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected");
    return false;
  }

  client.setInsecure();  // easier for testing. For production use setCACert()

  if (!client.connect(SMTP_HOST, SMTP_PORT)) {
    Serial.println("SMTP connection failed");
    return false;
  }

  // SMTP sequence with small delays to allow server response
  client.println("EHLO esp32");
  delay(150);

  client.println("AUTH LOGIN");
  delay(150);

  client.println(base64Encode(String(AUTHOR_EMAIL)));
  delay(150);

  client.println(base64Encode(String(AUTHOR_PASSWORD)));
  delay(200);

  client.println("MAIL FROM:<" + String(AUTHOR_EMAIL) + ">");
  delay(150);

  client.println("RCPT TO:<" + String(RECIPIENT_EMAIL) + ">");
  delay(150);

  client.println("DATA");
  delay(150);

  // Headers for HTML email
  client.println("From: Women Safety Device <" + String(AUTHOR_EMAIL) + ">");
  client.println("To: " + String(RECIPIENT_NAME) + " <" + String(RECIPIENT_EMAIL) + ">");
  client.println("Subject: " + subject);
  client.println("MIME-Version: 1.0");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("X-Priority: 1");
  client.println();  // blank line between headers and body

  // Body (already HTML)
  client.println(body);
  client.println(".");  // end of DATA
  client.println("QUIT");

  // Read server response
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 8000) {  // wait up to 8s for replies
    while (client.available()) {
      char c = client.read();
      resp += c;
    }
    if (resp.length()) {
      // break early if we see the final OK
      if (resp.indexOf("250") != -1 || resp.indexOf("235") != -1) break;
    }
    delay(50);
  }

  client.stop();

  if (resp.indexOf("250") != -1) {
    Serial.println("SMTP server response (contains 250):");
    Serial.println(resp);
    return true;
  } else {
    Serial.println("SMTP server response (no 250 found):");
    Serial.println(resp);
    return false;
  }
}

// --------- HTML email generator (kept your existing design) ----------
String createHTMLEmail(String reason) {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><style>";
  html += "body{font-family:Arial,sans-serif;background:#f5f5f5;margin:0;padding:20px}";
  html += ".container{max-width:600px;margin:0 auto;background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 4px 12px rgba(0,0,0,0.15)}";
  html += ".header{background:linear-gradient(135deg,#DC143C,#FF6B6B);color:#fff;padding:40px;text-align:center}";
  html += ".header h1{margin:0;font-size:32px;text-shadow:2px 2px 4px rgba(0,0,0,0.3)}";
  html += ".header p{margin:10px 0 0;font-size:16px;opacity:0.9}";
  html += ".content{padding:30px}";
  html += ".emergency{background:#FFF3CD;border:2px solid #FFC107;border-radius:8px;padding:20px;margin:20px 0;text-align:center}";
  html += ".emergency h2{color:#DC143C;margin:0 0 10px;font-size:24px}";
  html += ".detail{background:#f8f9fa;border-left:4px solid #DC143C;padding:15px;margin:15px 0;border-radius:4px}";
  html += ".label{font-weight:bold;color:#333;display:inline-block;min-width:120px}";
  html += ".value{color:#555}";
  html += ".instructions{background:#E3F2FD;border-left:4px solid #2196F3;padding:15px;margin:20px 0;border-radius:4px}";
  html += ".instructions h3{margin:0 0 10px;color:#1976D2}";
  html += ".instructions ol{margin:10px 0;padding-left:20px}";
  html += ".instructions li{margin:5px 0;color:#555}";
  html += ".footer{background:#333;color:#fff;padding:20px;text-align:center;font-size:13px}";
  html += ".footer p{margin:5px 0}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>🚨 EMERGENCY SOS ALERT</h1>";
  html += "<p>Women Safety Device - Immediate Response Required</p>";
  html += "</div>";
  html += "<div class='content'>";
  html += "<div class='emergency'>";
  html += "<h2>⚠ EMERGENCY DETECTED ⚠</h2>";
  html += "<p style='font-size:16px;color:#666;margin:5px 0'>Immediate attention required</p>";
  html += "</div>";
  html += "<div class='detail'>";
  html += "<span class='label'>Alert Type:</span>";
  html += "<span class='value'>" + reason + "</span>";
  html += "</div>";
  html += "<div class='detail'>";
  html += "<span class='label'>Timestamp:</span>";
  html += "<span class='value'>" + getTimestamp() + "</span>";
  html += "</div>";
  html += "<div class='detail'>";
  html += "<span class='label'>Device Status:</span>";
  html += "<span class='value'>Active & Monitoring</span>";
  html += "</div>";
  html += "<div class='instructions'>";
  html += "<h3>📋 Immediate Action Required:</h3>";
  html += "<ol>";
  html += "<li><strong>Contact the person immediately</strong> via phone call</li>";
  html += "<li><strong>Verify their safety</strong> and current situation</li>";
  html += "<li><strong>Ask for their location</strong> if they can respond</li>";
  html += "<li>If unreachable, <strong>contact local authorities</strong></li>";
  html += "<li>Share this alert with other emergency contacts</li>";
  html += "</ol>";
  html += "</div>";
  html += "<p style='margin-top:25px;padding:15px;background:#FFF;border:1px solid #ddd;border-radius:4px;color:#666;font-size:14px'>";
  html += "⚡ <strong>Note:</strong> This is an automated alert generated by the Women Safety IoT Device. ";
  html += "The device detected a potential emergency situation and triggered this notification. ";
  html += "Please respond immediately.";
  html += "</p>";
  html += "</div>";
  html += "<div class='footer'>";
  html += "<p><strong>Women Safety IoT Device</strong></p>";
  html += "<p>Emergency Alert System | Powered by ESP32</p>";
  html += "<p style='margin-top:10px;opacity:0.7'>This email was sent via secure SMTP connection</p>";
  html += "</div>";
  html += "</div></body></html>";

  return html;
}

String getTimestamp() {
  unsigned long seconds = millis() / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  seconds %= 60;
  minutes %= 60;
  hours %= 24;  // simple uptime format

  char buffer[30];
  sprintf(buffer, "Uptime: %02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buffer);
}