#include <Wire.h> //Need this for I2C
#include <EEPROM.h> //Need this for EEPROM Read/Write
#include <SPI.h> //Need this to talk to the radio
#include <LoRa.h> //https://github.com/sandeepmistry/arduino-LoRa

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

//These will help us keep track of how to respond to requests
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
  byte reliable;
  byte payloadLength;
  String payload;
} packet;

//Let's initialize a few instances of our new type
packet lastReceived = {0, 0, 0, 0, 0, 0, 0, ""};
packet lastSent = {0, 0, 0, 0, 0, 0, 0, ""};

//Counter for the Pairing Button Hold-down
uint16_t pair_hold = 0;

//This will help us keep track of how to respond to requests
byte responseType = RESPONSE_TYPE_STATUS;

void setup()
{
  //Set pin modes
  pinMode(PAIR_BTN, INPUT_PULLUP);
  pinMode(ADR_JUMPER, INPUT_PULLUP);
  pinMode(PAIR_LED, OUTPUT);
  pinMode(PWR_LED, OUTPUT);

  readSystemSettings(); //Load all system settings from EEPROM

  // override the default CS, reset, and IRQ pins (optional)
  LoRa.setPins(csPin, resetPin, irqPin);// set CS, reset, IRQ pin

  if (!LoRa.begin(915E6)) { //Initialize ratio at 915 MHz
    while (true) {
      //If failed, blink power LED
      digitalWrite(PWR_LED, 1);
      delay(500);
      digitalWrite(PWR_LED, 0);
      delay(500);
    };
  } else {
    //If successful, light power LED
    digitalWrite(PWR_LED, 1);
  }

  //Set our radio parameters from the stored values
  LoRa.setSyncWord(settingSyncWord);
  LoRa.setSpreadingFactor(settingSpreadFactor);
  LoRa.setTxPower(settingTXPower);

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

  //If the pairing button is being held
  if ( !digitalRead(PAIR_BTN) ) {
    pair_hold++;
    if (pair_hold > 2000) {
      pair_hold = 0;
      pairingSequence();
    }
  } else {
    pair_hold = 0;
  }

  onReceive(LoRa.parsePacket());
}

//Send a message via the radio
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
  lastSent.reliable = reliable;

  msgCount++;                           // increment message ID
}

//Check for a new radio packet, parse it, store it
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
    return; //Get outta here
  }

  //If the packet isn't addressed to this device or the broadcast channel,
  if (recipient != settingRFAddress && recipient != 0xFF) {
    return; //Get outta here
  }

  //If the message is for this device, or broadcast, store it appropriately:
  lastReceived.id = incomingMsgId;
  lastReceived.sender = sender;
  lastReceived.recipient = recipient;
  lastReceived.snr = LoRa.packetSnr();
  lastReceived.rssi = LoRa.packetRssi();
  lastReceived.payloadLength = incomingLength;
  lastReceived.payload = incoming;
  lastReceived.reliable = reliable;

  //If we're waiting on a Reliable Send ack, check to see if this is it.
  //A Reliable Send Ack payload has two bytes:
  //Byte 1: SNR of Reliable Packet
  //Byte 2: RSSI of Reliable Packet
  if ( (systemStatus >> 2) & 1 ) {

    //If the fourth byte of the header is equal to the sum of
    //the last sent message, we have Reliable Ack.
    if ( reliable == reliableSendChk ) {

      reliableSendChk = 0x00; //Reset Checksum accumulator
      reliableSendTime = 0x00; //Reset reliableSendTime timestamp
      reliableResend = 0x00; //Reset reliableResend timestamp
      systemStatus &= ~(1 << 2); //Clear "Waiting on Reliable Send" Status Flag
      systemStatus |= 1 << 0; //Set "Ready" Status Flag
      systemStatus &= ~(1 << 3); //Clear "Reliable Send Timeout" Status Flag

      //Grab these tidbits in case anyone wants them
      lastSent.snr = incoming.charAt(0);
      lastSent.rssi = incoming.charAt(1);

    }
  }

  //If this was a Reliable type message, calculate checksum and reply
  if ( reliable == 1 ) {

    String response = "";
    byte reliableAckChk = 0;
    //Calculate simple checksum of payload, reliableSendChk is type byte
    //so for sake of cycles, we will let it roll instead of explicitly
    //calculating (sum payload % 256)
    for ( int symbol = 0; symbol < incomingLength; symbol++ ) {
      reliableAckChk += incoming.charAt(symbol);
    }
    //Because we use values 0 and 1 for signalling, we ensure that the
    //checksum can never be < 1
    if ( reliableAckChk < 254 ) {
      reliableAckChk += 2;
    }
    response = lastReceived.snr;
    response += lastReceived.rssi;
    //Return to Sender
    sendMessage(sender, reliableAckChk, response);

  }

  //Set the "New Payload" Status Flag
  systemStatus |= 1 << 1;
}

//Read an I2C Request from the master
void receiveEvent(int numberOfBytesReceived)
{
  //Record bytes to local array
  byte incoming = Wire.read();

  //Set new I2C address
  if (incoming == COMMAND_SET_I2C_ADDRESS)
  {
    if (Wire.available())
    {
      settingI2CAddress = Wire.read();

      //Error check
      if (settingI2CAddress < 0x08 || settingI2CAddress > 0x77)
        return; //Command failed. This address is out of bounds.

      EEPROM.write(LOCATION_I2C_ADDR, settingI2CAddress);

      //Our I2C address may have changed because of user's command
      startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
    }
  }
  //Return the current system status
  else if (incoming == COMMAND_GET_STATUS)
  {
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Send a payload via the radio
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
  //Send a payload via the radio and request ack
  else if (incoming == COMMAND_SEND_RELIABLE)
  {

    byte recipient = Wire.read();

    String payload = "";

    while (Wire.available()) {
      payload += (char)Wire.read();
    }

    systemStatus &= ~(1 << 0); //Clear "Ready" Status Flag

    sendMessage(recipient, 1, payload);

    //Reset checksum accumulator
    reliableSendChk = 0;

    //Calculate simple checksum of payload, reliableSendChk is type byte
    //so for sake of cycles, we will let it roll instead of explicitly
    //calculating (sum payload % 256)
    for ( int symbol = 0; symbol < payload.length(); symbol++ ) {
      reliableSendChk += payload.charAt(symbol);
    }

    //Because we use values 0 and 1 for signalling, we ensure that the
    //checksum can never be < 1
    if ( reliableSendChk < 254 ) {
      reliableSendChk += 2;
    }

    reliableSendTime = millis();
    reliableResend = millis();

    systemStatus |= 1 << 2; //Set "Waiting on Reliable Send" Status Flag

  }
  //Set the time in seconds to wait for reliable ack before failing
  else if (incoming == COMMAND_SET_RELIABLE_TIMEOUT)
  {

    settingMessageTimeout = Wire.read();
    EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);

  }
  //Return the payload of the last received packet
  else if (incoming == COMMAND_GET_PAYLOAD)
  {

    responseType = RESPONSE_TYPE_PAYLOAD;

  }
  //Set the spread factor of the radio
  else if (incoming == COMMAND_SET_SPREAD_FACTOR)
  {

    byte newSpreadFactor = Wire.read();

    if (newSpreadFactor > 12 || newSpreadFactor < 6) {
      return;
    }

    settingSpreadFactor = newSpreadFactor;
    EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);
    LoRa.setSpreadingFactor(settingSpreadFactor);

  }
  //Set the Sync Word of the radio
  else if (incoming == COMMAND_SET_SYNC_WORD)
  {

    settingSyncWord = Wire.read();
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    LoRa.setSyncWord(settingSyncWord);

  }
  //Set the Address of the radio
  else if (incoming == COMMAND_SET_RF_ADDRESS)
  {

    byte newRFAddress = Wire.read();

    if (newRFAddress < 0xFF) { //0xFF is Broadcast Channel
      settingRFAddress = newRFAddress;
      EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
    }

  }
  //Return the Address of the radio
  else if (incoming == COMMAND_GET_RF_ADDRESS)
  {

    responseType = RESPONSE_TYPE_RF_ADDRESS;
    return;

  }
  //Return the RSSI of the last received packet
  else if (incoming == COMMAND_GET_PACKET_RSSI)
  {

    responseType = RESPONSE_TYPE_PACKET_RSSI;
    return;

  }
  //Return the size of the payload of the last received packet
  else if (incoming == COMMAND_GET_PAYLOAD_SIZE)
  {

    responseType = RESPONSE_TYPE_PACKET_SIZE;
    return;

  }
  //Return the origin address of the last received packet
  else if (incoming == COMMAND_GET_PACKET_SENDER)
  {

    responseType = RESPONSE_TYPE_PACKET_SENDER;
    return;

  }
  //Return the destination address of the last received packet
  else if (incoming == COMMAND_GET_PACKET_RECIPIENT)
  {

    responseType = RESPONSE_TYPE_PACKET_RECIPIENT;
    return;

  }
  //Return the SNR (Signal-to-Noise Ratio) of the last received packet
  else if (incoming == COMMAND_GET_PACKET_SNR)
  {

    responseType = RESPONSE_TYPE_PACKET_SNR;
    return;

  }
  //Return the sequential ID of the last received packet
  else if (incoming == COMMAND_GET_PACKET_ID)
  {

    responseType = RESPONSE_TYPE_PACKET_ID;
    return;

  }
  //Adjust the radio transmit amplifier
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

//Respond to I2C master's request for bytes
void requestEvent()
{
  //Return system status byte
  if (responseType == RESPONSE_TYPE_STATUS)
  {
    Wire.write(systemStatus);
  }
  //Return payload of last received packet
  else if (responseType == RESPONSE_TYPE_PAYLOAD)
  {
    char payload[256];
    lastReceived.payload.toCharArray(payload, 256);
    Wire.write(payload, lastReceived.payloadLength);
    responseType = RESPONSE_TYPE_STATUS;
    systemStatus &= ~(1 << 1); //Clear "New Payload" Status Flag
  }
  //Return current local address
  else if (responseType == RESPONSE_TYPE_RF_ADDRESS)
  {
    Wire.write(settingRFAddress);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the RSSI of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_RSSI)
  {
    Wire.write(lastReceived.rssi);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the size of the payload in the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SIZE)
  {
    Wire.write(lastReceived.payloadLength);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the origin address of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SENDER)
  {
    Wire.write(lastReceived.sender);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the destination address of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_RECIPIENT)
  {
    Wire.write(lastReceived.recipient);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the SNR (Signal-to-Noise Ratio) of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_SNR)
  {
    Wire.write(lastReceived.snr);
    responseType = RESPONSE_TYPE_STATUS;
  }
  //Return the sequential ID of the last received packet
  else if (responseType == RESPONSE_TYPE_PACKET_ID)
  {
    Wire.write(lastReceived.id);
    responseType = RESPONSE_TYPE_STATUS;
  }
  else //By default we respond with the system status byte
  {
    Wire.write(systemStatus);
  }
}

//Restore radio parameters or defaults
void readSystemSettings(void)
{

  //If this is the first ever boot, or EEPROM was nuked, load defaults to EEPROM:
  if ( EEPROM.read(LOCATION_RADIO_ADDR) == 0xFF ) {

    EEPROM.write(LOCATION_I2C_ADDR, I2C_ADDRESS_DEFAULT);
    EEPROM.write(LOCATION_RADIO_ADDR, settingRFAddress);
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    EEPROM.write(LOCATION_SPREAD_FACTOR, settingSpreadFactor);
    EEPROM.write(LOCATION_MESSAGE_TIMEOUT, settingMessageTimeout);
    EEPROM.write(LOCATION_TX_POWER, settingTXPower);

    //If not, load radio paramters from EEPROM
  } else {

    settingRFAddress = EEPROM.read(LOCATION_RADIO_ADDR);
    settingSyncWord = EEPROM.read(LOCATION_SYNC_WORD);
    settingSpreadFactor = EEPROM.read(LOCATION_SPREAD_FACTOR);
    settingMessageTimeout = EEPROM.read(LOCATION_MESSAGE_TIMEOUT);
    settingTXPower = EEPROM.read(LOCATION_TX_POWER);

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

//Pairing Sequence:
//1) Hold Pairing button on radio 1 until pairing LED turns on and then off again.
//2) Hold Pairing button on radio 2 until pairing LED turns on and then off again.
//3) Both Pairing LEDs will blink rapidly when paired
//Pairing results in two radios aggreeing to jump to a new random Sync Word
void pairingSequence(void)
{
  //Turn on the pairing LED to signal the start of the pairing sequence
  digitalWrite(PAIR_LED, 1);
  //Set SyncWord to known value for pairing
  LoRa.setSyncWord(0x00);
  //Begin building pairing advertisement
  String advertise = "###";
  //Seed PRNG with wideband RSSI
  randomSeed(LoRa.random());
  //Randomly generate a new SyncWord
  byte newSyncWord = random(255);
  //Add new SyncWord to advertisement
  advertise += newSyncWord;
  //This flag will be used to skip the advertising stage if we're radio 2
  bool paired = 0;

  //Before we advertise, let's listen to see if we're radio 2
  for (unsigned long listening = millis() ; millis() < listening + 3000 ; ) {

    paired = pairingParser(LoRa.parsePacket());

  }

  //Now that we're done listening, we assume we're radio 1. So turn off the
  //pairing LED to signal the user it's time to press the pairing button on radio 2
  digitalWrite(PAIR_LED, 0);

  //Until we get a pairing ack, keep sending advertisements on the broadcast channel
  //with SyncWord 0x00 and then switching to the advertised SyncWord to listen for
  //an ack. Only do this if we didn't pair up as someone's radio 2.
  while ( !paired && !pairingParser(LoRa.parsePacket()) ) {

    LoRa.setSyncWord(0x00);
    sendMessage(0xFF, 0, advertise);
    LoRa.setSyncWord(newSyncWord);

  }

  //Now that we have a friend with the same SyncWord, write it to EEPROM
  settingSyncWord = newSyncWord;
  EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);

  //Blink rapidly to alert user that pairing was successful
  for ( uint8_t blinky = 0 ; blinky < 5 ; blinky++ ) {

    digitalWrite(PAIR_LED, 1);
    delay(250);
    digitalWrite(PAIR_LED, 0);
    delay(250);

  }
}

//The pairing parser is a stripped down version of the usual packet parser
//which doesn't check the reliable packet status of incoming packets,
//change the system status flags, or store packets to lastReceived. It's
//used only for validating packets and searching their payloads for
//pairing advertisements and acks
bool pairingParser(int packetSize) {

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
    return 0; //Get outta here
  }

  //If the packet isn't addressed to this device or the broadcast channel,
  if (recipient != settingRFAddress && recipient != 0xFF) {
    return 0; //Get outta here
  }

  //Check if it's a pairing request and send an ack if it is
  if ( incoming.substring(0, 3) == "###" ) {
    settingSyncWord = incoming.charAt(3);
    EEPROM.write(LOCATION_SYNC_WORD, settingSyncWord);
    LoRa.setSyncWord( settingSyncWord );
    sendMessage(sender, 0, "$$$");
    return 1;
  }

  //Check if it's a pairing ack and return to pairing sequence if it is
  if ( incoming.substring(0, 3) == "$$$" ) {
    return 1;
  }

  //If the packet isn't for us, is malformed, or isn't any kind of pairing message,
  return 0;

}

