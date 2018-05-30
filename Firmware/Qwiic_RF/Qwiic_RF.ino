//TODO
//Pairing Sequence

#include <Wire.h>
#include <EEPROM.h>
#include <SPI.h>
#include <LoRa.h>
#include <avr/sleep.h>
#include <avr/power.h>

//Location in EEPROM where various settings will be stored
#define LOCATION_I2C_ADDR 0x01
#define LOCATION_RADIO_ADDR 0x02
#define LOCATION_SYNC_WORD 0x03
#define LOCATION_SPREAD_FACTOR 0x04
#define LOCATION_MESSAGE_TIMEOUT 0x05
#define LOCATION_TX_POWER 0x06

//There is an ADR jumpber on this board. When closed, forces I2C address to default.
#define I2C_ADDRESS_DEFAULT 0x35
#define I2C_ADDRESS_JUMPER_CLOSED 0x36

//These are the commands we understand and may respond to
#define COMMAND_GET_STATUS 0x01
#define COMMAND_SEND 0x02
#define COMMAND_SEND_RELIABLE 0x03
#define COMMAND_SET_RELIABLE_TIMEOUT 0x04
#define COMMAND_GET_PAYLOAD 0x05
#define COMMAND_SET_SPREAD_FACTOR 0x06
#define COMMAND_SET_SYNC_WORD 0x07
#define COMMAND_SET_RF_ADDRESS 0x08
#define COMMAND_GET_RF_ADDRESS 0x09
#define COMMAND_GET_PACKET_RSSI 0x0A
#define COMMAND_GET_PAYLOAD_SIZE 0x0B
#define COMMAND_GET_PACKET_SENDER 0x0C
#define COMMAND_GET_PACKET_RECIPIENT 0x0D
#define COMMAND_GET_PACKET_SNR 0x0E
#define COMMAND_GET_PACKET_ID 0x0F
#define COMMAND_SET_TX_POWER 0x10
#define COMMAND_SET_I2C_ADDRESS 0x20

#define RESPONSE_TYPE_STATUS 0x00
#define RESPONSE_TYPE_PAYLOAD 0x01
#define RESPONSE_TYPE_RF_ADDRESS 0x02
#define RESPONSE_TYPE_PACKET_RSSI 0x03
#define RESPONSE_TYPE_PACKET_SIZE 0x04
#define RESPONSE_TYPE_PACKET_SENDER 0x05
#define RESPONSE_TYPE_PACKET_RECIPIENT 0x06
#define RESPONSE_TYPE_PACKET_SNR 0x07
#define RESPONSE_TYPE_PACKET_ID 0x08

//A Few Pin Definitions
#define ADR_JUMPER 4
#define PAIR_BTN A1
#define PAIR_LED A2
#define PWR_LED A3

//Firmware version. This is sent when requested. Helpful for tech support.
const byte firmwareVersionMajor = 0;
const byte firmwareVersionMinor = 5;

//RFM95 pins
const int csPin = 10;          // LoRa radio chip select
const int resetPin = 9;       // LoRa radio reset
const int irqPin = 3;         // change for your board; must be a hardware interrupt pin

//*** These get twiddled during interrupts so they're type volatile ***

//System status variable
//Bit 0 - Ready To Send
//Bit 1 - Packet Available
//Bit 2 - Waiting on Reliable Send
//Bit 3 - Reliable Send Timeout
volatile byte systemStatus = 0b00000000;
//This global is used to keep track of the I2C Address (Software Switchable)
volatile byte settingI2CAddress = I2C_ADDRESS_DEFAULT;
//These globals are used for the "Reliable Send" routine
volatile unsigned long reliableSendTime = 0x00;
volatile unsigned long reliableResend = 0x00;
volatile byte reliableSendChk = 0x00;

//*** ***

//These are the radio parameters that are used to 
//Initialize the RFM95. We assign them default
//values so we can load EEPROM on first boot.
byte settingRFAddress = 0xBB;
byte settingSyncWord = 0x00;
byte settingSpreadFactor = 0x07;
byte settingMessageTimeout = 0x1E;
byte settingTXPower = 0x11;
byte msgCount = 0x00;

//This is a type for storing radio packets
typedef struct {
  byte id;
  byte sender;
  byte recipient;
  byte snr;
  byte rssi;
  byte payloadLength;
  String payload;
} packet;

//Let's initialize a few instances of our new type
packet lastReceived = {0, 0, 0, 0, 0, 0, ""};
packet lastSent = {0, 0, 0, 0, 0, 0, ""};

void setup()
{
  //Set pin modes
  pinMode(PAIR_BTN, INPUT_PULLUP);
  pinMode(ADR_JUMPER, INPUT_PULLUP);
  pinMode(PAIR_LED, OUTPUT);
  pinMode(PWR_LED, OUTPUT);

  //If this is the first ever boot, or EEPROM was nuked, load defaults to EEPROM:
  if ( EEPROM.read(LOCATION_RADIO_ADDR) == 0xFF ) {

    EEPROM.write(LOCATION_I2C_ADDR, I2C_ADDRESS_DEFAULT);
    EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);
    EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);
    EEPROM.write(LOCATION_TX_POWER, settingTXPower);

  } else {

    settingRFAddress = EEPROM.read(LOCATION_RADIO_ADDR);
    settingSyncWord = EEPROM.read(LOCATION_SYNC_WORD);
    settingSpreadFactor = EEPROM.read(LOCATION_SPREAD_FACTOR);
    settingMessageTimeout = EEPROM.read(LOCATION_MESSAGE_TIMEOUT);
    settingTXPower = EEPROM.read(LOCATION_TX_POWER);

  }

  // override the default CS, reset, and IRQ pins (optional)
  LoRa.setPins(csPin, resetPin, irqPin);// set CS, reset, IRQ pin

  readSystemSettings(); //Load all system settings from EEPROM

  if (!LoRa.begin(915E6)) {             // initialize ratio at 915 MHz
    while (true) {
      // if failed, blink status LED
    };
  }

  //Begin listening on I2C only after we've setup all our config and opened any files
  startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
  systemStatus |= 1 << 0; //Set "Ready" Status Flag
}

void loop()
{
  //If there is a "Waiting on Reliable Send" flag and the ack timer has expired,
  //remove the flag and set the "Reliable Send Timeout" flag instead.
  if ( ( (systemStatus >> 2) & 1 ) && ( millis() > ( reliableSendTime * 1000 ) + settingMessageTimeout ) ) {
    systemStatus &= ~(1 << 2); //Clear "Waiting on Reliable Send" Status Flag
    systemStatus |= 1 << 0; //Set "Ready" Status Flag
    systemStatus |= 1 << 3; //Set "Reliable Send Timeout" Status Flag
    reliableSendChk = 0x00; //Reset Checksum accumulator
    reliableSendTime = 0x00; //Reset reliableSendTime timestamp
    reliableResend = 0x00; //Reset reliableResend timestamp
    //If there is a "Waiting on Reliable Send" flag and the ack timer has NOT expired,
    //check the reliable send interval counter and possibly try resending.
  } else if ( ( (systemStatus >> 2) & 1 ) && ( millis() < ( reliableSendTime * 1000 ) + settingMessageTimeout ) ) {
    if ( millis() > reliableResend + 1000 ) { //Retry once per second
      sendMessage(lastSent.recipient, 0, lastSent.payload);
      reliableResend = millis();
    }
  }

  onReceive(LoRa.parsePacket());
}

void sendMessage(byte destination, byte reliable, String outgoing) 
{
  LoRa.beginPacket();                   // start packet
  LoRa.write(destination);              // add destination address
  LoRa.write(settingRFAddress);         // add sender address
  LoRa.write(msgCount);                 // add message ID
  LoRa.write(reliable);                 // add reliable send tag
  LoRa.write(outgoing.length());        // add payload length
  LoRa.print(outgoing);                 // add payload
  LoRa.endPacket();                     // finish packet and send it

  lastSent.id = msgCount;
  lastSent.sender = settingRFAddress;
  lastSent.recipient = destination;
  lastSent.payloadLength = outgoing.length();
  lastSent.payload = outgoing;

  msgCount++;                           // increment message ID
}

void onReceive(int packetSize) {
  if (packetSize == 0) return;          // if there's no packet, return

  // read packet header bytes:
  int recipient = LoRa.read();          // recipient address
  byte sender = LoRa.read();            // sender address
  byte incomingMsgId = LoRa.read();     // incoming msg ID
  byte reliable = LoRa.read();          // reliable send tag
  byte incomingLength = LoRa.read();    // incoming msg length

  String incoming = "";

  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  if (incomingLength != incoming.length()) {   // check length for error
    return;                             // skip rest of function
  }

  // if the recipient isn't this device or broadcast,
  if (recipient != localAddress && recipient != 0xFF) {
    return;                             // skip rest of function
  }

  // if message is for this device, or broadcast, print details:

  lastReceived.id = incomingMsgId;
  lastReceived.sender = sender;
  lastReceived.recipient = recipient;
  lastReceived.snr = LoRa.packetSnr();
  lastReceived.rssi = LoRa.packetRssi();
  lastReceived.payloadLength = incomingLength;
  lastReceived.payload = incoming;

  //If we're waiting on a Reliable Send ack, check to see if this is it.
  //A Reliable Send Ack payload has three bytes:
  //Byte 0: Reliable Ack Checksum
  //Byte 1: SNR of Reliable Packet
  //Byte 2: RSSI of Reliable Packet
  if ( (systemStatus >> 2) & 1 ) {

    //If the first byte of the payload is equal to the sum of
    //The last sent message, we have Reliable Ack.
    if ( payload.charAt(0) == reliableSendChk ) {

      reliableSendChk = 0x00; //Reset Checksum accumulator
      reliableSendTime = 0x00; //Reset reliableSendTime timestamp
      reliableResend = 0x00; //Reset reliableResend timestamp
      systemStatus &= ~(1 << 2); //Clear "Waiting on Reliable Send" Status Flag
      systemStatus |= 1 << 0; //Set "Ready" Status Flag
      systemStatus &= ~(1 << 3); //Clear "Reliable Send Timeout" Status Flag

      //Grab these tidbits in case anyone wants them
      lastSent.snr = payload.charAt(1);
      lastSent.rssi = payload.charAt(2);

    }

  }

  //If this was a Reliable type message, calculate sum%255 and reply
  if ( reliable == 1 ) {

    String response = "";
    byte reliableAckChk = 0;
    //Calculate simple checksum of payload, reliableSendChk is type byte
    //so for sake of cycles, we will let it roll instead of explicitly
    //calculating (sum payload % 255)
    for ( int symbol = 0; symbol < incomingLength; symbol++ ) {
      reliableAckChk += incoming.charAt(symbol);
    }
    response = reliableAckChk;
    response += lastReceived.snr;
    response += lastReceived.rssi;
    //Return to Sender
    sendMessage(sender, 0, response);

  }

  systemStatus |= 1 << 1; //Set "New Payload" Status Flag
}

void receiveEvent(int numberOfBytesReceived)
{
  //Record bytes to local array
  byte incoming = Wire.read();

  if (incoming == COMMAND_SET_I2C_ADDRESS) //Set new I2C address
  {
    if (Wire.available())
    {
      settingI2CAddress = Wire.read();

      //Error check
      if (settingI2CAddress < 0x08 || settingI2CAddress > 0x77)
        return; //Command failed. This address is out of bounds.

      EEPROM.write(LOCATION_I2C_ADDRESS, settingI2CAddress);

      //Our I2C address may have changed because of user's command
      startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
    }
  }
  else if (incoming == COMMAND_GET_STATUS)
  {
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (incoming == COMMAND_SEND)
  {

    byte recipient = Wire.read();

    String payload = "";

    while (Wire.available()) {
      payload += (char)Wire.read();
    }

    systemStatus &= ~(1 << 0); //Clear "Ready" Status Flag

    sendMessage(recipient, 0, payload);

    systemStatus |= 1 << 0; //Set "Ready" Status Flag

  }
  else if (incoming == COMMAND_SEND_RELIABLE)
  {

    byte recipient = Wire.read();

    String payload = "";

    while (Wire.available()) {
      payload += (char)Wire.read();
    }

    systemStatus &= ~(1 << 0); //Clear "Ready" Status Flag

    sendMessage(recipient, 1, payload);

    //Calculate simple checksum of payload, reliableSendChk is type byte
    //so for sake of cycles, we will let it roll instead of explicitly
    //calculating (sum payload % 255)
    for ( int symbol = 0; symbol < payload.length(); symbol++ ) {
      reliableSendChk += payload.charAt(symbol);
    }

    reliableSendTime = millis();
    reliableResend = millis();

    systemStatus |= 1 << 2; //Set "Waiting on Reliable Send" Status Flag

  }
  else if (incoming == COMMAND_SET_RELIABLE_TIMEOUT)
  {

    settingMessageTimeout = Wire.read();
    EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);

  }
  else if (incoming == COMMAND_GET_PAYLOAD)
  {

    responseType = RESPONSE_TYPE_PAYLOAD;

  }
  else if (incoming == COMMAND_SET_SPREAD_FACTOR)
  {

    byte newSpreadFactor = Wire.read();

    if (newSpreadFactor > 12 || newSpreadFactor < 6) {
      return;
    }

    settingSpreadFactor = newSpreadFactor;
    EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);

  }
  else if (incoming == COMMAND_SET_SYNC_WORD)
  {

    settingSyncWord = Wire.read();
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);

  }
  else if (incoming == COMMAND_SET_RF_ADDRESS)
  {

    byte newRFAddress = Wire.read();

    if (newRFAddress < 0xFF) { //0xFF is Broadcast Channel
      settingRFAddress = newRFAddress;
      EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
    }

  }
  else if (incoming == COMMAND_GET_RF_ADDRESS)
  {

    responseType = RESPONSE_TYPE_RF_ADDRESS;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_RSSI)
  {

    responseType = RESPONSE_TYPE_PACKET_RSSI;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_SIZE)
  {

    responseType = RESPONSE_TYPE_PACKET_SIZE;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_SENDER)
  {

    responseType = RESPONSE_TYPE_PACKET_SENDER;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_RECIPIENT)
  {

    responseType = RESPONSE_TYPE_PACKET_RECIPIENT;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_SNR)
  {

    responseType = RESPONSE_TYPE_PACKET_SNR;
    return;

  }
  else if (incoming == COMMAND_GET_PACKET_ID)
  {

    responseType = RESPONSE_TYPE_PACKET_ID;
    return;

  }
  else if (incoming == COMMAND_SET_TX_POWER)
  {

    byte txPower = Wire.read();
    if (txPower > 17) {
      txPower = 17;
    }
    settingTXPower = txPower;
    EEPROM.write(LOCATION_TX_POWER, settingTXPower);
    LoRa.setTxPower(txPower);

  }
}

void requestEvent()
{
  if (responseType == RESPONSE_TYPE_STATUS)
  {
    Wire.write(systemStatus);
  }
  else if (responseType == RESPONSE_TYPE_PAYLOAD)
  {
    Wire.write(lastReceived.payload, lastReceived.payloadLength);
    responseType = RESPONSE_TYPE_STATUS;
    systemStatus &= ~(1 << 1); //Clear "New Payload" Status Flag
  }
  else if (responseType == RESPONSE_TYPE_RF_ADDRESS)
  {
    Wire.write(settingRFAddress);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_RSSI)
  {
    Wire.write(lastReceived.rssi);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_SIZE)
  {
    Wire.write(lastReceived.payloadLength);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_SENDER)
  {
    Wire.write(lastReceived.sender);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_RECIPIENT)
  {
    Wire.write(lastReceived.recipient);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_SNR)
  {
    Wire.write(lastReceived.snr);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else if (responseType == RESPONSE_TYPE_PACKET_ID)
  {
    Wire.write(lastReceived.id);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else //By default we respond with the result from the last operation
  {
    Wire.write(systemStatus);
  }
}

void readSystemSettings(void)
{
  //Read what I2C address we should use
  settingI2CAddress = EEPROM.read(LOCATION_I2C_ADDRESS);
  if (settingI2CAddress == 255)
  {
    settingI2CAddress = I2C_ADDRESS_DEFAULT; //By default, we listen for I2C_ADDRESS_DEFAULT
    EEPROM.write(LOCATION_I2C_ADDRESS, settingI2CAddress);
  }
}

//Begin listening on I2C bus as I2C slave using the global variable setting_i2c_address
void startI2C()
{
  Wire.end(); //Before we can change addresses we need to stop

  if (digitalRead(ADR_JUMPER) == HIGH) //Default is HIGH.
    Wire.begin(settingI2CAddress); //Start I2C and answer calls using address from EEPROM
  else //User has closed jumper with solder to GND
    Wire.begin(I2C_ADDRESS_JUMPER_CLOSED); //Force address to I2C_ADDRESS_NO_JUMPER if user has opened the solder jumper

  //The connections to the interrupts are severed when a Wire.begin occurs. So re-declare them.
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}

void pairingSequence(void) 
{
  


  
}

