#include <Wire.h>
#include <EEPROM.h>
#include <SPI.h>
#include <LoRa.h>

#include <avr/sleep.h> //Needed for sleep_mode
#include <avr/power.h> //Needed for powering down perihperals such as the ADC/TWI and Timers

//Location in EEPROM where various settings will be stored
#define LOCATION_I2C_ADDR 0x01
#define LOCATION_RADIO_ADDR 0x02
#define LOCATION_SYNC_WORD 0x03

//There is an ADR jumpber on this board. When closed, forces I2C address to a given address.
#define I2C_ADDRESS_DEFAULT 0x
#define I2C_ADDRESS_JUMPER_CLOSED 0x

//These are the commands we understand and may respond to
#define COMMAND_GET_STATUS 0x01
#define COMMAND_SEND 0x02
#define COMMAND_SEND_RELIABLE 0x03
#define COMMAND_SET_RELIABLE_TIMEOUT 0x04
#define COMMAND_GET_PAYLOAD 0x05
#define COMMAND_SET_SPREAD_FACTOR 0x06
#define COMMAND_SET_SYNC_WORD 0x07
#define COMMAND_SET_CHANNEL 0x08
#define COMMAND_GET_CHANNEL 0x09
#define COMMAND_GET_PACKET_RSSI 0x0A
#define COMMAND_GET_PACKET_SIZE 0x0B
#define COMMAND_GET_PACKET_SENDER 0x0C
#define COMMAND_GET_PACKET_RECIPIENT 0x0D
#define COMMAND_GET_PACKET_SNR 0x0E
#define COMMAND_GET_PACKET_ID 0x0F
#define COMMAND_SET_TX_POWER 0x10

#define RESPONSE_TYPE_STATUS 0x00
#define RESPONSE_TYPE_PAYLOAD 0x01
#define RESPONSE_TYPE_CHANNEL 0x02
#define RESPONSE_TYPE_PACKET_RSSI 0x03
#define RESPONSE_TYPE_PACKET_SIZE 0x04
#define RESPONSE_TYPE_PACKET_SENDER 0x05
#define RESPONSE_TYPE_PACKET_RECIPIENT 0x06
#define RESPONSE_TYPE_PACKET_SNR 0x07
#define RESPONSE_TYPE_PACKET_ID 0x08

#define TASK_NONE 0x00

//Firmware version. This is sent when requested. Helpful for tech support.
const byte firmwareVersionMajor = 1;
const byte firmwareVersionMinor = 0;

//Hardware pins
const int csPin = 10;          // LoRa radio chip select
const int resetPin = 9;       // LoRa radio reset
const int irqPin = 3;         // change for your board; must be a hardware interrupt pin

//Variables used in the I2C interrupt so we use volatile
volatile byte systemStatus = SYSTEM_STATUS_OK; //Tracks the response from the MP3 IC

volatile byte settingAddress = I2C_ADDRESS_DEFAULT; //The 7-bit I2C address of this QMP3
volatile byte settingVolume = 0;
volatile byte settingEQ = 0;

void setup()
{
  // override the default CS, reset, and IRQ pins (optional)
  LoRa.setPins(csPin, resetPin, irqPin);// set CS, reset, IRQ pin

  readSystemSettings(); //Load all system settings from EEPROM

    if (!LoRa.begin(915E6)) {             // initialize ratio at 915 MHz
    while (true){
      // if failed, blink status LED
    };
  }

  //Begin listening on I2C only after we've setup all our config and opened any files
  startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
}

void loop()
{

}


void receiveEvent(int numberOfBytesReceived)
{
  //Record bytes to local array
  byte incoming = Wire.read();

  if (incoming == COMMAND_SET_ADDRESS) //Set new I2C address
  {
    if (Wire.available())
    {
      settingAddress = Wire.read();

      //Error check
      if (settingAddress < 0x08 || settingAddress > 0x77)
        return; //Command failed. This address is out of bounds.

      EEPROM.write(LOCATION_I2C_ADDRESS, settingAddress);

      //Our I2C address may have changed because of user's command
      startI2C(); //Determine the I2C address we should be using and begin listening on I2C bus
    }
  }
  else if (incoming == COMMAND_STOP)
  {

  }

}

void requestEvent()
{
  if (responseType == )
  {
    Wire.write();
  }
  else if (responseType == )
  {
    
  }
  else //By default we respond with the result from the last operation
  {
    Wire.write(systemStatus);
  }

}

void readSystemSettings(void)
{
  //Read what I2C address we should use
  settingAddress = EEPROM.read(LOCATION_I2C_ADDRESS);
  if (settingAddress == 255)
  {
    settingAddress = I2C_ADDRESS_DEFAULT; //By default, we listen for I2C_ADDRESS_DEFAULT
    EEPROM.write(LOCATION_I2C_ADDRESS, settingAddress);
  }
}

//Begin listening on I2C bus as I2C slave using the global variable setting_i2c_address
void startI2C()
{
  Wire.end(); //Before we can change addresses we need to stop

  if (digitalRead(adr) == HIGH) //Default is HIGH.
    Wire.begin(settingAddress); //Start I2C and answer calls using address from EEPROM
  else //User has closed jumper with solder to GND
    Wire.begin(I2C_ADDRESS_JUMPER_CLOSED); //Force address to I2C_ADDRESS_NO_JUMPER if user has opened the solder jumper

  //The connections to the interrupts are severed when a Wire.begin occurs. So re-declare them.
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}
