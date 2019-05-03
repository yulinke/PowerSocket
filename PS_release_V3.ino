#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <SPI.h>

ESP8266WebServer server(80);

//--Define ASC71020 parameter-------
#define InputVoltage 140          //Input voltage is 120Vrms, Vpeak is 170V .
#define FullScaleCurrent 30       //ACS71020KMABTR-015B5-SPI, full current is 30A.
//--Define MQTT parameter-------
//#define MqttPublishID "TestPowerSocket2"            //Set MQTT publish ID.
#define MqttPublishID "PowerSocket3"            //Set MQTT publish ID.
#define MqttSubscribeID "ControlPowerSocket3"   //Set MQTT Subscribe ID, the message "0" is power off, the "1" is power on.
//--Define Power information parameter-------
#define SampleTimes 1500          //Detail information sample times.
#define ByPassTimes 15000         //Bypass 15000 times, send information once time.(15000:30s)
#define CurrentThreshold 0.07     //Over Current Threshold, send detail power information.

/*** SPI Controller ***************************************************************************************
  - Every ESP8266 device is different but for NodeMCU and HUZZAH apparently
   7280000 is the sweet spot.
  - MSBFIRST is the default and figures 6-1 and 6-2 in the docs assume it anyway.
  - MODE0 means low low clock mode. We're hoping to catch the bits on the falling edge of the oscillation.
   Learn about some SPI clocks. It's interesting stuff.

  /**************SPI MODE*********************************
   | SPI MODE |Clock polarity |Clock phase  |
   |          | (CPOL/CKP)    |   (CPHA)    |
   |    0     |     0         |     0       |
   |    1     |     0         |     1       |
   |    2     |     1         |     0       |
   |    3     |     1         |     1       |
***********************************************************************************************************/

SPISettings spiSettings(100000, LSBFIRST, SPI_MODE2);
const int SPI_CS = 15;          //Define ESP8266 GPIO-15 is SPI chip select.
const int RelayControl = 5;     //Define relay control pin is GPIO-5.

// Update these with values suitable for your network.
const char* ssid = "PowerSocketWiFi ";
const char* password = "test";

// MQTT server IP
const char* mqtt_server = "MQTT-server-IP";

WiFiClient espClient;
PubSubClient client(espClient);

int flag = 0;     //State machine control.
int count = 0;
int continuous = 0;
double RefanceCurrent = 0;  //Save the current value, compared with new current values.

//Wifi-AP-STA
String st;
String content;

int statusCode;

void setup() {
    Serial.begin(115200);
    EEPROM.begin(512);
    delay(10);

    pinMode(BUILTIN_LED, OUTPUT);       // Initialize the BUILTIN_LED pin as an output
    pinMode(SPI_CS, OUTPUT);                // Initialize the GPIO-15 as an output
    pinMode(RelayControl, OUTPUT);      // Initialize the GPIO-5 as an output
    
    setup_wifi();
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
    
    SPI.begin();
    SPI.setBitOrder(LSBFIRST);
    SPI.setFrequency(10000);
    
    digitalWrite(BUILTIN_LED, LOW);     // Turn the LED on by making the voltage LOW.
    digitalWrite(RelayControl, HIGH);       // Turn the relay and power on.
    
    delay(50);
}


void loop() {
//  if (testWifi()) { launchWeb(0);  }


  if (!client.connected()) {  reconnectMQTT();  }
  client.loop();  //function client.loop() should be called regularly to allow the client to process incoming messages to send publish data and makes a refresh of the connection.
  if (!digitalRead(RelayControl)) { flag = 3; }

  switch (flag) {
    case 0:     //No load.
//    
//      Serial.print(" flag:");
//      Serial.print(flag);
//      Serial.print(" count:");
//      Serial.print(count);
//      Serial.print(" continuous:");
//      Serial.print(continuous);
//      Serial.println("");
//      
      if (ReadIrms() < 0.1) {
        if (count < ByPassTimes) {  //I < 0.05A continuous 30 times, avoid noise.
          flag = 0;         //Keep flag status.
          count ++;
          continuous = 0;   //reset continuous.
        } else {
          SendNoLoadData();
          flag = 0;         //Keep flag status.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.
        }
      } else {
        if (continuous < 4) {       //I > 0.05A continuous two times, avoid noise.
          continuous ++;
          flag = 0;         //Keep flag status.
        } else {
          SendDetailData();
          flag = 1;         //I>0.05A, has load.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.
        }
      } break;

    case 1:     //Identification appliance
//
//      Serial.print(" flag:");
//      Serial.print(flag);
//      Serial.print(" count:");
//      Serial.print(count);
//      Serial.print(" continuous:");
//      Serial.print(continuous);

      if (ReadIrms() < 0.05) {      //Removed the load.
        if (continuous < 10) {      //I < 0.05A continuous three times, avoid noise.
          SendDetailData();
          continuous ++;
          flag = 1;         //Keep flag status.
          //          count = 0;        //reset count.
        } else {
          SendNoLoadData();
          flag = 0;         //removed the load.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.
        }
      } else {
        if (count < SampleTimes) {
          SendDetailData();
          count ++;
          flag = 1;         //Keep flag status.
          continuous = 0;   //reset continuous.
        } else {
          RefanceCurrent = ReadIrms();  //save curent value, cample with new current.
          SendStableData();
          flag = 2;         //change to stable sample rate.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.

        }
      } break;

    case 2:     //Stable mode, if current change over 0.05A change to case 1.
//
//      Serial.print(" flag:");
//      Serial.print(flag);
//      Serial.print(" count:");
//      Serial.print(count);
//      Serial.print(" continuous:");
//      Serial.print(continuous);
//      Serial.print(" RefanceCurrent:");
//      Serial.print(RefanceCurrent);
//      Serial.println("");
//

      if ((RefanceCurrent - ReadIrms()) > CurrentThreshold || (ReadIrms() - RefanceCurrent) > CurrentThreshold) { //Load change.
        SendDetailData();
        flag = 1;           //Load change, get detail information.
        count = 0;          //reset count.
        continuous = 0;     //reset continuous.
      } else if (ReadIrms() < 0.04) {
        SendNoLoadData();
        flag = 0;           //Load change, get detail information.
        count = 0;        //reset count.
        continuous = 0;   //reset continuous.
      } else {
        if (count < ByPassTimes) {
          count ++;
          flag = 2;         //Keep flag status.
          continuous = 0;   //reset continuous.
        } else {
          SendStableData();
          flag = 2;         //Keep flag status.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.
        }
      } break;

    case 3:     //power off mode.
//
//      Serial.print(" flag:");
//      Serial.print(flag);
//      Serial.print(" count:");
//      Serial.print(count);
//      Serial.print(" continuous:");
//      Serial.print(continuous);
//      Serial.print(" RefanceCurrent:");
//      Serial.print(RefanceCurrent);
//      Serial.println("");
//

      if (digitalRead(RelayControl)) {  //Relay status is 1.
        flag = 1;
        count = 0;        //reset count.
        continuous = 0;   //reset continuous.
      } else {
        if (count < ByPassTimes) {
          count ++;
          flag = 3;         //Keep flag status.
          continuous = 0;   //reset continuous.
        } else {
//          SendPowerOffData();
          flag = 3;         //Keep flag status.
          count = 0;        //reset count.
          continuous = 0;   //reset continuous.
        }
      } break;

    default:
      flag = 0;
      count = 0;            //reset count.
      continuous = 0;       //reset continuous.
      RefanceCurrent = 0;
      break;
  }

  //  //-------------add delay and clean flush------------------
//  Serial.println("");
  Serial.flush();
  client.flush();
  //  delay(1000);
}

void SendNoLoadData() {
  String msgStr = "";

  Serial.print("Publish message: ");
  msgStr = "{\"S\":0";    //Status 0 is no load.
  msgStr += ",\"T\":";
  msgStr += millis();
  msgStr += ",\"V\":";
  msgStr += ReadVrms();
  msgStr += ",\"A\":0";
  msgStr += ",\"PF\":100";
  msgStr += ",\"W\":0";
  msgStr += ",\"VA\":0";
  msgStr += ",\"VAR\":0}";

  byte arrSize = msgStr.length() + 1;
  char msg[arrSize];

  Serial.println(msgStr);

  msgStr.toCharArray(msg, arrSize);   //change msgStr data format, from string change to char array.
  client.publish(MqttPublishID, msg);
}

void SendDetailData() {
  String msgStr = "";

  Serial.print("Publish message: ");
  msgStr = "{\"S\":1";    //Status 1 is identification appliances mode.
  msgStr += ",\"T\":";
  msgStr += millis();
  msgStr += ",\"V\":";
  msgStr += ReadVrms();
  msgStr += ",\"A\":";
  msgStr += ReadIrms();
  msgStr += ",\"PF\":";
  msgStr += ReadPowerFactor();
  msgStr += ",\"W\":";
  msgStr += ReadActivePower();
  msgStr += ",\"VA\":";
  msgStr += ReadApparentPower();
  msgStr += ",\"VAR\":";
  msgStr += ReadReactivePower();
  msgStr += "}";

  byte arrSize = msgStr.length() + 1;
  char msg[arrSize];

  Serial.println(msgStr);

  msgStr.toCharArray(msg, arrSize);   //change msgStr data format, from string change to char array.
  client.publish(MqttPublishID, msg);
}

void SendStableData() {
  String msgStr = "";

  Serial.print("Publish message: ");
  msgStr = "{\"S\":2";    //Status 2 is stable mode.
  msgStr += ",\"T\":";
  msgStr += millis();
  msgStr += ",\"V\":";
  msgStr += ReadVrms();
  msgStr += ",\"A\":";
  msgStr += ReadIrms();
  msgStr += ",\"PF\":";
  msgStr += ReadPowerFactor();
  msgStr += ",\"W\":";
  msgStr += ReadActivePower();
  msgStr += ",\"VA\":";
  msgStr += ReadApparentPower();
  msgStr += ",\"VAR\":";
  msgStr += ReadReactivePower();
  msgStr += "}";

  byte arrSize = msgStr.length() + 1;
  char msg[arrSize];

  Serial.println(msgStr);

  msgStr.toCharArray(msg, arrSize);   //change msgStr data format, from string change to char array.
  client.publish(MqttPublishID, msg);
}

void SendPowerOffData() {
  String msgStr = "";

  Serial.print("Publish message: ");
  msgStr = "{\"S\":3";    //Status 3 is Power off.
  msgStr += ",\"T\":";
  msgStr += millis();
  msgStr += ",\"V\":0";
  msgStr += ",\"A\":0";
  msgStr += ",\"PF\":0";
  msgStr += ",\"W\":0";
  msgStr += ",\"VA\":0";
  msgStr += ",\"VAR\":0";
  msgStr += "}";

  byte arrSize = msgStr.length() + 1;
  char msg[arrSize];

  Serial.println(msgStr);

  msgStr.toCharArray(msg, arrSize);   //change msgStr data format, from string change to char array.
  client.publish(MqttPublishID, msg);
}

/******ACS71020 power monitoring control**************************************
   Read Power Factor value function.
   This field is a unsigned 9-bit fixed point number with 9 fractional bits.
   Adress 0x24 data 10:0 bits is active power output.
   ActivePower=(ADCData/2^15)*InputVoltage*FullScaleCurrent.
 *****************************************************************************/
double ReadPowerFactor () {
  uint8_t PowerFactorAddress = 0x24;
  int PowerFactorBitNumber = 9;
  int *Add24Data;
  int PowerFactorADCFullScale = 9;
  int PowerFactorBitSum = 0;
  double ReturnPowerFactor = 0;    //return power factor.

  ReadData(&PowerFactorAddress);
  Add24Data = ReadData(&PowerFactorAddress);

  for (int i = 0; i < PowerFactorBitNumber; i++)
    PowerFactorBitSum += ((*(Add24Data + i)) * pow(2, i));

  ReturnPowerFactor = (PowerFactorBitSum / pow(2, PowerFactorADCFullScale)) * 100;

  return ReturnPowerFactor;
}

/*******ACS71020 power monitoring control**************************************
   Read reactive power output function.
   This apparent is a unsigned 17-bit fixed point number with 16 fractional bits.
   Adress 0x23 data 16:0 bits is active power output.

 *****************************************************************************/
double ReadReactivePower () {
  uint8_t ReactivePowerAddress = 0x23;
  int ReactivePowerBitNumber = 17;
  int *Add23Data;
  int ReactivePowerADCFullScale = 16;
  int ReactivePowerBitSum = 0;
  double ReactivePower = 0;

  ReadData(&ReactivePowerAddress);
  Add23Data = ReadData(&ReactivePowerAddress);

  for (int i = 0; i < ReactivePowerBitNumber; i++)
    ReactivePowerBitSum += ((*(Add23Data + i)) * pow(2, i));

  ReactivePower = ((ReactivePowerBitSum / pow(2, ReactivePowerADCFullScale)) * InputVoltage) * FullScaleCurrent;

  return ReactivePower;
}

/*******ACS71020 power monitoring control**************************************
   Read apparent power output function.
   This apparent is a unsigned 16-bit fixed point number with 15 fractional bits.
   Adress 0x22 data 15:0 bits is active power output.

 *****************************************************************************/
double ReadApparentPower () {
  uint8_t ApparentPowerAddress = 0x22;
  int ApparentPowerBitNumber = 16;
  int *Add22Data;
  int ApparentPowerADCFullScale = 15;
  int ApparentPowerBitSum = 0;
  double ApparentPower = 0;

  ReadData(&ApparentPowerAddress);
  Add22Data = ReadData(&ApparentPowerAddress);

  for (int i = 0; i < ApparentPowerBitNumber; i++)
    ApparentPowerBitSum += ((*(Add22Data + i)) * pow(2, i));

  ApparentPower = ((ApparentPowerBitSum / pow(2, ApparentPowerADCFullScale)) * InputVoltage) * FullScaleCurrent;

  return ApparentPower;
}

/*******ACS71020 power monitoring control**************************************
   Read active power output function.
   This field is a signed 17-bit fixed point number with 15 fractional bits.
   Adress 0x21 data 16:0 bits is active power output.
   ActivePower=(ADCData/2^15)*InputVoltage*FullScaleCurrent.
 *****************************************************************************/
double ReadActivePower () {
  double ActivePower = 0;

  ActivePower = sqrt(pow(ReadApparentPower(), 2) - pow(ReadReactivePower(), 2));

  /********************************************
    uint8_t Address = 0x21;
    int BitNumber = 17;
    int *AddData;
    int ADCFullScale = 15;
    int BitValue = 0;
    double ActivePower = 0;

    AddData = ReadData(&Address);

    for (int i = 0; i < BitNumber; i++)
      BitValue += ((*(AddData + i)) * pow(2, i));

    ActivePower = ((BitValue / pow(2, ADCFullScale)) * InputVoltage) * FullScaleCurrent;
  */

  return ActivePower;
}

/*******ACS71020 power monitoring control**************************************
   The same time read  voltage RMS and current RMS value.
   Adress 0x20 data 14:0 bits is Vrms.
   Adress 0x20 data 30:16 bits is Irms.
   Voltage=(ADCData/2^15)*InputVoltage.
   Current=(ADCData/2^14)*FullScaleCurrent.
 *****************************************************************************/
void ReadIVrms () {
  uint8_t Address = 0x20;
  int BitNumber = 15;
  int *Add20Data;
  int VrmsADCFullScale = 15;
  int IrmsADCFullScale = 14;
  int VrmsBitSum = 0;
  int IrmsBitSum = 0;
  double Vrms = 0;
  double Irms = 0;

  ReadData(&Address);
  Add20Data = ReadData(&Address);

  for (int i = 0; i < BitNumber; i++) {
    VrmsBitSum += ((*(Add20Data + i)) * pow(2, i));
    IrmsBitSum += ((*(Add20Data + (i + 16))) * pow(2, i));
  }

  Vrms = (VrmsBitSum / pow(2, VrmsADCFullScale)) * InputVoltage;
  Irms = (IrmsBitSum / pow(2, IrmsADCFullScale)) * FullScaleCurrent;
}

/*******ACS71020 power monitoring control**************************************
   Read current RMS value function.
   Adress 0x20 data 30:16 bits is Irms,
   Current=(ADCData/2^14)*FullScaleCurrent.
 ******************************************************************************/
double ReadIrms () {
  uint8_t Address = 0x20;
  int *AddData;
  int ADCFullScale = 14;
  int BitSum = 0;
  double ReturnValue = 0;    //Return current value.

  ReadData(&Address);
  AddData = ReadData(&Address);

  for (int i = 16, j = 0; i < 31; i++, j++)
    BitSum += (*(AddData + i)) * pow(2, j);

  ReturnValue = (BitSum / pow(2, ADCFullScale)) * FullScaleCurrent;

  return ReturnValue;
}

/*******ACS71020 power monitoring control**************************************
   Read voltage RMS value function.
   Adress 0x20 data 14:0 bits is Vrms,
   Voltage=(ADCData/2^15)*InputVoltage.
 ******************************************************************************/
double ReadVrms () {
  uint8_t Address = 0x20;
  int VrmsBitNumber = 15;
  int *Add20Data;
  int VrmsADCFullScale = 15;
  int VrmsBitSum = 0;
  double ReturnVrms = 0;    //Return voltage value.

  ReadData(&Address);
  Add20Data = ReadData(&Address);

  for (int i = 0; i < VrmsBitNumber; i++)
    VrmsBitSum += ((*(Add20Data + i)) * pow(2, i));

  ReturnVrms = VrmsBitSum / pow(2, VrmsADCFullScale) * InputVoltage;

  return ReturnVrms;
}

/*******ACS71020 power monitoring control**************************************
   Read instantaneous voltage value function.
   Adress 0x2A data 16:0 bits is instantaneous voltage.
   Voltage=(ADCData/2^16)*InputVoltage.
   17-bit=1 is negative output.
 ******************************************************************************/
double ReadVoltage () {
  uint8_t Address = 0x2A;
  int BitNumber = 16;
  int *AddData;
  int ADCFullScale = 16;
  int BitSum = 0;
  double ReturnValue = 0;

  AddData = ReadData(&Address);

  for (int i = 0; i < BitNumber; i++)
    BitSum += ((*(AddData + i)) * pow(2, i));

  if (*(AddData + 16) == 1)
    ReturnValue = BitSum / pow(2, ADCFullScale) * InputVoltage;
  else
    ReturnValue = BitSum / pow(2, ADCFullScale) * InputVoltage * -1;

  return ReturnValue;
}

/*******ACS71020 power monitoring control**************************************
   Read instantaneous current value function.
   Adress 0x2B data 16:0 bits is instantaneous current.
   Current=(ADCData/2^15)*FullScaleCurrent.
   17-bit=1 is negative output.
   16-bit=1 is ????.
 ******************************************************************************/
double ReadCurrent () {
  uint8_t Address = 0x2B;
  int BitNumber = 16;
  int *AddData;
  int ADCFullScale = 15;
  int BitSum = 0;
  double ReturnValue = 0;

  AddData = ReadData(&Address);

  for (int i = 0; i < BitNumber; i++)
    BitSum += ((*(AddData + i)) * pow(2, i));

  Serial.print(*(AddData + 16));
  Serial.print(":");
  Serial.print(*(AddData + 15));
  Serial.print(":");
  Serial.print(BitSum);
  Serial.print(":");
  if (*(AddData + 16) == 1)
    ReturnValue = BitSum / pow(2, ADCFullScale) * FullScaleCurrent;
  else
    ReturnValue = BitSum / pow(2, ADCFullScale) * FullScaleCurrent * -1;

  return ReturnValue;
}

/*******ACS71020 power monitoring control**************************************
   Input register address, freedback 32-bits register data.
    -Read data the 8-bit have to change to high.
    -Write data the 8-bit have to change to low.
********************************************************************************/
int * ReadData (uint8_t * Address) {
  byte ver[4];
  static int bits[32];
  uint8_t ReadAddress = *Address + 0x80; // Read data 8-bit have to change to high.

  SPI.beginTransaction(spiSettings);    // Setting up SPI.

  digitalWrite (SPI_CS, LOW);           // CS to low, start teansfer data.

  SPI.transfer(ReadAddress);            // Teansfer register address

  for (int i = 0; i < 4; i++) {          // Receive 32-bits data.
    ver[i] = SPI.transfer(0);
  }

  digitalWrite (SPI_CS, HIGH);          // CS to high, end teansfer data.

  SPI.endTransaction();                 // Close SPI.

  int j = 0;                            // Transform byte to bit, and combined data string.
  for (int y = 0; y < 4; y++) {
    for (int k = 0; k < 8; k++) {
      bits[j] = bitRead(ver[y], k);
      j++;
    }
  }
  return bits;
}

/******Set WiFi connect********************************

******************************************************/
void setup_wifi() {
    Serial.println("Startup");
    // read eeprom for ssid and pass
 
    Serial.println("Reading EEPROM ssid");
    String esid;
    for (int i = 0; i < 32; ++i) {
        esid += char(EEPROM.read(i));
    }
    Serial.print("SSID: ");
    Serial.println(esid);

    Serial.println("Reading EEPROM pass");
    String epass = "";
    for (int i = 32; i < 96; ++i) {
        epass += char(EEPROM.read(i));
    }
    Serial.print("PASS: ");
    Serial.println(epass); 
    
    WiFi.mode(WIFI_STA);    //Setting to Station mode
    delay(1000);
    WiFi.begin(esid.c_str(), epass.c_str());

    if (!testWifi()) {   setupAP(); }

    delay(10);
    // We start by connecting to a WiFi network
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(esid);
    
    randomSeed(micros());
    
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP()); 
}



/***** WiFi setup *****************************************

*/
bool testWifi(void) {
    int c = 0;
    Serial.println("Waiting for Wifi to connect"); 
    while ( c < 20 ) {
        if (WiFi.status() == WL_CONNECTED) { return true; }
        delay(500);
        Serial.print(WiFi.status());   
        c++;
    }
    Serial.println("");
    Serial.println("Connect timed out, opening AP");
    return false;
}

void launchWeb(int webtype) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
    createWebServer(webtype);
    // Start the server
    server.begin();
    Serial.println("Server started");
}

void setupAP(void) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    Serial.println("scan done");
    if (n == 0) {
        Serial.println("no networks found");
    } else {
        Serial.print(n);
        Serial.println(" networks found");
        
        for (int i = 0; i < n; ++i) {
            // Print SSID and RSSI for each network found
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.print(WiFi.SSID(i));
            Serial.print(" (");
            // Gets the signal strength of the connection to the router
            Serial.print(WiFi.RSSI(i));
            Serial.print(")");
            //Gets the encryption type of the current network
            //TKIP (WPA) = 2, WEP = 5, CCMP (WPA) = 4, NONE = 7, AUTO = 8
            Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE)?"open":"*");
            delay(10);
        }
    }
    Serial.println("");
    
    //Print network to web page 
    st = "<ol>";
    for (int i = 0; i < n; ++i)
    {
        // Print SSID and RSSI for each network found
        st += "<li>";
        st += WiFi.SSID(i);
        st += " (";
        st += WiFi.RSSI(i);
        st += ")";
        st += (WiFi.encryptionType(i) == ENC_TYPE_NONE)?"open":"*";
        st += "</li>";
    }
    st += "</ol>";
    delay(100);
    
//    WiFi.softAP(ssid, password, 6);
    WiFi.mode(WIFI_AP); //AP mode
    WiFi.softAP(ssid);
    Serial.print("softap SSID: ");
    Serial.println(ssid);
    launchWeb(1);
    Serial.println("over");
}

void createWebServer(int webtype)
{
    if ( webtype == 1 ) {
        server.on("/", []() {
            IPAddress ip = WiFi.softAPIP();
            String ipStr = String(ip[0]) + '.' + String(ip[1]) + '.' + String(ip[2]) + '.' + String(ip[3]);
            content = "<!DOCTYPE HTML>\r\n<html>Hello from ESP8266 at ";
            content += ipStr;
            content += "<p>";
            content += st;
            content += "</p><form method='get' action='setting'><label>SSID: </label><input name='ssid' length=32><input type='password' name='pass' length=64><input type='submit'></form>";
            content += "</html>";
            server.send(200, "text/html", content); 
        });
        
        server.on("/setting", []() {
            String qsid = server.arg("ssid");
            String qpass = server.arg("pass");
            if (qsid.length() > 0 && qpass.length() > 0) {
                Serial.println("clearing eeprom");
                for (int i = 0; i < 96; ++i) { EEPROM.write(i, 0); }
                Serial.println(qsid);
                Serial.println("");
                Serial.println(qpass);
                Serial.println("");
                
                Serial.println("writing eeprom ssid:");
                for (int i = 0; i < qsid.length(); ++i)
                {
                    EEPROM.write(i, qsid[i]);
                    Serial.print("Wrote: ");
                    Serial.println(qsid[i]);
                }

                Serial.println("writing eeprom pass:");
                for (int i = 0; i < qpass.length(); ++i)
                {
                    EEPROM.write(32+i, qpass[i]);
                    Serial.print("Wrote: ");
                    Serial.println(qpass[i]);
                }   

                EEPROM.commit();
                content = "{\"Success\":\"saved to eeprom... reset to boot into new wifi\"}";
                statusCode = 200;
                ESP.restart();
            } else {
                content = "{\"Error\":\"404 not found\"}";
                statusCode = 404;
                Serial.println("Sending 404");
            }
            server.send(statusCode, "application/json", content);
        });

    } else if (webtype == 0) {
        server.on("/", []() {
            IPAddress ip = WiFi.localIP();
            String ipStr = String(ip[0]) + '.' + String(ip[1]) + '.' + String(ip[2]) + '.' + String(ip[3]);
            server.send(200, "application/json", "{\"IP\":\"" + ipStr + "\"}");
        });

        server.on("/cleareeprom", []() {
            content = "<!DOCTYPE HTML>\r\n<html>";
            content += "<p>Clearing the EEPROM</p></html>";
            server.send(200, "text/html", content);
            Serial.println("clearing eeprom");
            for (int i = 0; i < 96; ++i) { EEPROM.write(i, 0); }
            EEPROM.commit();
        });
    }
}



/****** MQTT arrived data ***********************************
   Use this function control socket ON/OFF
   user send topic "ControlPowerSocket1" control GPIO
 ************************************************************/
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // Switch on the LED if an 1 was received as first character
  if ((char)payload[0] == '0') {
    digitalWrite(BUILTIN_LED, HIGH);   // Turn the LED off.
    digitalWrite(RelayControl, LOW);   // Turn the power off.
  } else if((char)payload[0] == '1'){
    digitalWrite(BUILTIN_LED, LOW);     // Turn the LED on.
    digitalWrite(RelayControl, HIGH);   // Turn the power on.
  }
}

/***** MQTT connect *****************************************

*/
void reconnectMQTT() {
    // Loop until we're reconnected
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        
        // Create a random client ID
        String clientId = "ESP8266Client-";
        clientId += String(random(0xffff), HEX);
        
        // Attempt to connect
        if (client.connect(clientId.c_str())) {
            Serial.println("connected");
            // Once connected, publish an announcement...
            //      client.publish(MqttPublishID, "hello world");
            // ... and resubscribe
            client.subscribe(MqttSubscribeID);
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
            
            while (WiFi.status() != WL_CONNECTED) {
                delay(500);
                Serial.print(".");
                server.handleClient();
            }
        }
    }
}
