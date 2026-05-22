/*
 * Don't Forget to Set
 * 
 * Firmware for the MAX32630FTHR
 * 
 * Alistair MacDonald 2026 
 * 
 */

#include <pwrseq_regs.h>
#include "CC256XB.h"
#include <Ethernet2.h>


// Settings

#define MAC_ADDRESS_COUNT 3
#define MAC_ADDRESS_SIZE 6

// Bluetooth tracker MAC addresses
uint8_t macAddressList[MAC_ADDRESS_COUNT][MAC_ADDRESS_SIZE] = {
  {0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x01}, // BLE Tracker 1
  {0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x02}, // BLE Tracker 2
  {0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x03}  // BLE Tracker 3
};

// Time to wait for tracker to be confirmed absent
#define TAG_REPEAT_DURATION 120000

// The MAC address for our Ethernent interface
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// Address to call when trackers are absent 
#define HTTP_HOST "www.virtualsmarthome.xyz"
#define HTTP_URN "/url_routine_trigger/activate.php?trigger=f4d8dc11-e745-428b-8868-d7dc6f234cc0&token=30a03f63-8a49-4e9f-9e95-956302f43d0f"


// Globals and Constants

// PAN1326C2 HCI UART (use Serial 0 on MAX32630FTHR)
#define BT_SERIAL Serial0
// PAN1326C2 reset pin
#define BT_RST P1_6

// Timeout for HCI commands
#define HCI_TIMEOUT 1000

// Timeout for PAN1326C2 to respond after boot
#define HARDWARE_BOOT_TIME 1400

// Delay to alow PAN1326C2 and 32768Hz oscillator to settle
#define HARDWARE_SETTLE_TIME 200

// HCI Command to reset the PAN1326C2
const byte hciCommandReset[] = {0x01, 0x03, 0x0C, 0x00};

// When did we last see one of our tags
unsigned long lastFoundTime = -TAG_REPEAT_DURATION;

// Had the tracker been found in the last loop itteration
int lastFoundRecently = false;

// The Ethernet client interface
EthernetClient client;


// Functions

// Setup routine
void setup() {
  
  // Setu up the LED pins for debugging feedback
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(RED_LED, LOW); 

  // Open serial for debugging
  Serial.begin(115200);
  Serial.println("Starting...");

  // Set pins for PAN1326B communication to lov voltave opperation
  useVDDIO(P0_0);
  useVDDIO(P0_1);
  useVDDIO(P0_2);
  useVDDIO(P0_3);
  useVDDIO(P1_6);
  useVDDIO(P1_7);

  // Reset (and hold) the PAN1326B
  pinMode(BT_RST, OUTPUT);
  digitalWrite(BT_RST, LOW);

  // Enable the 32768Hz Oscillator Output on P1.7 (from 4.5.1.5.2)
  MXC_PWRSEQ->reg4 |= MXC_F_PWRSEQ_REG4_PWR_PSEQ_32K_EN;
  delay(HARDWARE_SETTLE_TIME);

  // Initialize UART0 for PAN1326B communication
  BT_SERIAL.begin(115200);
  // Swap the RX/TX lines and enable CTS & RTS
  MXC_IOMAN->uart0_req |= MXC_F_IOMAN_UART0_REQ_CTS_IO_REQ | MXC_F_IOMAN_UART0_REQ_RTS_IO_REQ | MXC_F_IOMAN_UART0_REQ_IO_MAP;

  // Enable the PAN1326B
  // Note that P1_6 may already be high (input with an internal pullup) after we enables the oscillator output
  digitalWrite(BT_RST, HIGH);
  delay(HARDWARE_SETTLE_TIME);

  // Send the software reset HCI command
  BT_SERIAL.write(hciCommandReset, sizeof(hciCommandReset));

  // Wait for responce (and reset if it has stalled)
  while (!BT_SERIAL.available()) {
    if ( millis() > HARDWARE_BOOT_TIME ) {
      Serial.println("Stalled so rebooting...");
      delay(HARDWARE_SETTLE_TIME);
      // Reset the ARM core
      NVIC_SystemReset();
      // We should have rebooted by now
    }
    delay(HARDWARE_SETTLE_TIME);
  }

  // Skip over the respocnce to the HCI reset command
  receiveHCIResponse(BT_SERIAL);

  // Upload the PAN1326C2 firmware patch and initialisation commands
  Serial.println("Sending PAN1326C2 firmware patch and initalisation commands");
  sendServicePack(CC256XB_PATCH_HCI, sizeof(CC256XB_PATCH_HCI));
  Serial.println("Patch and commands sent successfully.");

  // Initalise the Ethernet
  Serial.println("Connecting to network...");
  Ethernet.init(P5_4);
  Ethernet.begin(mac);
  Serial.print( "Local IP address : " );
  Serial.println( Ethernet.localIP() );

  // Setup complete
  digitalWrite(RED_LED, HIGH); 

}

// A function to flash one of the LEDs for debugging
void flashLED(int inLED, int inDuration=20) {
    digitalWrite(inLED, LOW); 
    delay(inDuration);
    digitalWrite(inLED, HIGH); 
}

// Make an HTTP GET call
void sendHTTP(String inHost, String inURN) {
  if (client.connect(inHost.c_str(), 80)) {
    Serial.println("Connected to " + inHost);
    // Make the HTTP request:
    client.println("GET " + inURN + " HTTP/1.0");
    client.println("Host: " + inHost);
    client.println("Connection: close");
    client.println();
    // Wait and clsoe as we don't need a responce
    delay(1000);
    client.stop();
  }
  else {
    Serial.println("Error connecting to server...");
  }
}

// A blocking stream read function with timeout
int streamReadBlocking(Stream &inSerial, int inTimeout = HCI_TIMEOUT) {
  unsigned long waitEnd = millis() + inTimeout;
  while ( ( !inSerial.available() ) && ( waitEnd>millis() ) ) {
    delay(1);
  }
  return inSerial.read();
}

// A blcking function to skip an HCI command responce
int streamSkipBlocking(Stream &inSerial, int inLength, int inTimeout = HCI_TIMEOUT) {
  unsigned long waitEnd = millis() + inTimeout;
  int numRead = 0;
  while ( ( numRead<inLength ) && ( waitEnd>millis() ) ) {
    if ( inSerial.available() ) {
      inSerial.read();
      numRead++;
    }
    else {
      delay(1);
    }
  }
  return numRead;
}

// A function to preocess an HCI low energy advertising report
void receiveHCILEAdvertisingReport(Stream &inSerial, uint8_t eventCode, uint8_t paramLength) {

  // Read the remainder of the header
  uint8_t Subevent_Code = streamReadBlocking(inSerial);
  uint8_t Num_Reports = streamReadBlocking(inSerial);

  // Loop thougth the reports contained in the report
  for (int reportNum=0; reportNum<Num_Reports; reportNum++) {

    // Read the report data
    uint8_t Event_Type = streamReadBlocking(inSerial);
    uint8_t Address_Type = streamReadBlocking(inSerial);
    uint8_t Address[MAC_ADDRESS_SIZE];
    for (int i=MAC_ADDRESS_SIZE-1; i>=0; i--) {
      Address[i] = streamReadBlocking(inSerial);
    }
    uint8_t Length = streamReadBlocking(inSerial);
    streamSkipBlocking(inSerial, Length);
    uint8_t RSSI = streamReadBlocking(inSerial);

    // Print the address for debugging
    Serial.print("Found device ");
    Serial.print(Address[0], HEX);
    for (int j=1; j<MAC_ADDRESS_SIZE; j++) {
      Serial.print(":");
      Serial.print(Address[j], HEX);
    }

    // Identify if one of these is one of our tags
    int anyTagFound = false;
    for (int tag=0; tag<MAC_ADDRESS_COUNT; tag++) {
      int tagMatched = true;
      for (int k=0; k<MAC_ADDRESS_SIZE; k++) {
        tagMatched &= ( macAddressList[tag][k] == Address[k] );
      }
      anyTagFound |= tagMatched;
    }

    // Rememeber the time last found if found
    if (anyTagFound) {
      lastFoundTime = millis();
      Serial.print(" Match");
    } 

    // Print the RSSI for debugging
    Serial.print(" (");
    Serial.print(RSSI);
    Serial.println(")");


  }
}

// A dispatcher function to preocess an HCI responce
void receiveHCICommandResponse(Stream &inSerial, uint8_t eventCode, uint8_t paramLength) {
  switch (eventCode) {
    case 0x3e: // 0x3E for LE event
        receiveHCILEAdvertisingReport(inSerial, eventCode, paramLength);
      break;
    // Functions for other responces can be added here
    default: // Skip what we don't understand
      streamSkipBlocking(inSerial, paramLength, HCI_TIMEOUT);
  }
}

// A function to receave and decode an HCI responce
void receiveHCIResponse(Stream &inSerial) {
  uint8_t packetType  = streamReadBlocking(inSerial);
  uint8_t eventCode   = streamReadBlocking(inSerial);
  uint8_t paramLength = streamReadBlocking(inSerial);    
  if (packetType==4) {
    // Process the responce if a command responce
    receiveHCICommandResponse(inSerial, eventCode, paramLength);
  }
  else {
    // Skip what we don't understand
    streamSkipBlocking(inSerial, paramLength, HCI_TIMEOUT);
  }
  // Flash an LED for debugging
  flashLED(BLUE_LED);
}


// A funtion to send a patch (and initialisation HCI commands) to the PAN1326C2
void sendServicePack(const uint8_t patchBinary[], uint16_t totalSize) {

  uint16_t cursor = 0;
  while (cursor < totalSize) {

    // Send the header
    BT_SERIAL.write(pgm_read_byte(&patchBinary[cursor++])); // Command (should always be 0x01)
    BT_SERIAL.write(pgm_read_byte(&patchBinary[cursor++])); // Opcode Low
    BT_SERIAL.write(pgm_read_byte(&patchBinary[cursor++])); // Opcode High

    // Send the size
    uint8_t blockSize = pgm_read_byte(&patchBinary[cursor++]); // Read the size of the block
    BT_SERIAL.write(blockSize);

    // Send the actual data
    for (int i=0; i<blockSize; i++) {
      BT_SERIAL.write(pgm_read_byte(&patchBinary[cursor++]));
    }

    // Wait for a responce
    receiveHCIResponse(BT_SERIAL);

  }
}

// The main loop
void loop() {

  // Process any data sent from the PAN1326C2
  if (BT_SERIAL.available()) {
    receiveHCIResponse(BT_SERIAL);
  }

  // Check if a tag has been seen recently
  unsigned long lastFoundAgo = millis()-lastFoundTime;
  int foundRecently = lastFoundAgo < TAG_REPEAT_DURATION;
  digitalWrite(GREEN_LED, !foundRecently); 

  // Have all trackers gone just now?
  if (lastFoundRecently && !foundRecently) {
    // Make the call to arm the alarm
    sendHTTP(HTTP_HOST, HTTP_URN);
    lastFoundRecently = false;
  }

  // Reset last found recently if round recently set
  lastFoundRecently |= foundRecently;

}
