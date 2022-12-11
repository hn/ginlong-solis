/*

   solis2influx.ino

   ESP8266 gateway to read Ginlong Solis inverter stats and statistics and push to influxdb

   (C) 2022 Hajo Noerenberg

   http://www.noerenberg.de/
   https://github.com/hn/ginlong-solis

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program. If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.

*/

#define INFLUXDB_URL      "http://192.168.0.123:8086"
#define INFLUXDB_DB_NAME  "smarthome"
#define INFLUXDB_MEAS     "solis"

#define MODBUSPINTX       D6
#define MODBUSPINRX       D5
#define MODBUSBAUD        9600
#define MODBUSINVERTERID  1

#define WIFINAME          "SOLIS2INFLUX"
#define SNHEXWORDS        4
#define SNASCWORDS        16

#include <WiFiManager.h>
#include <InfluxDbClient.h>
#include <SoftwareSerial.h>
#include <ModbusMaster.h>

enum solisdatatype {
  SDT_U16,
  SDT_U32,
  SDT_S16,
  SDT_S32,
  SDT_H16,    // Hex number
  SDT_ITYPE,  // Inverter type definition
  SDT_SNHEX,  // Serial number, 4 words hex encoded
  SDT_SNASC,  // Serial number, 16 words direct ASCII
  SDT_APP1,   // Appendix 1 - Product model
  SDT_APP2,   // Appendix 2 - Inverter status
  SDT_APP3,   // Appendix 3 - Grid standard
  SDT_APP4,   // Appendix 4 - Power curve number
  SDT_APP5,   // Appendix 5 - Fault status
  SDT_APP6,   // Appendix 6 - Working status
  SDT_APP7,   // Appendix 7
  SDT_APP8,   // Appendix 8 - Setting flag
};

enum modbusobjecttype {
  MB_COIL,            // 1 bit r/w   func 0x05 read - Solis '0X' type
  MB_DISCRETEINPUT,   // 1 bit r     func 0x02 read - Solis '1X' type
  MB_INPUTREG,        // 16 bit r    func 0x04 read - Solis '3X' type
  MB_HOLDINGREG,      // 16 bit r/w  func 0x03 read, 0x06 write and 0x10 write multiple - Solis '4X' type
};

typedef struct {
  modbusobjecttype modbusobject;
  unsigned int modbusaddr;
  unsigned int modbusoffset;
  unsigned int readdelay;           // poll frequency in seconds, -1 to disable
  solisdatatype datatype;
  unsigned int datadiv;
  char *dataunit;
  char *dataname;
} solisreg;

/* RS485_MODBUS map while inverter type is unknown */
solisreg solisUNKNOWN[] = {
  { MB_INPUTREG,  35000,  0,     20,   SDT_ITYPE,    1,      "",  "Solis inverter type definition" },
  {}
};

/* RS485_MODBUS (INV-3000ID EPM-36000ID) inverter protocol */
solisreg solisINV[] = {
  { MB_INPUTREG,   3000,  1,    300,    SDT_APP1,    1,      "",  "Product model" },
  { MB_INPUTREG,   3005,  1,     60,     SDT_U32,    1,     "W",  "Active power" },
  { MB_INPUTREG,   3007,  1,     60,     SDT_U32,    1,     "W",  "Total DC output power" },
  { MB_INPUTREG,   3009,  1,     60,     SDT_U32,    1,   "kWh",  "Total energy" },
  { MB_INPUTREG,   3011,  1,     90,     SDT_U32,    1,   "kWh",  "Energy this month" },
  { MB_INPUTREG,   3013,  1,     90,     SDT_U32,    1,   "kWh",  "Energy last month" },
  { MB_INPUTREG,   3015,  1,     90,     SDT_U16,   10,   "kWh",  "Energy today" },
  { MB_INPUTREG,   3016,  1,     90,     SDT_U16,   10,   "kWh",  "Energy last day" },
  { MB_INPUTREG,   3022,  1,     30,     SDT_U16,   10,     "V",  "DC voltage 1" },
  { MB_INPUTREG,   3023,  1,     30,     SDT_U16,   10,     "A",  "DC current 1" },
  { MB_INPUTREG,   3024,  1,     30,     SDT_U16,   10,     "V",  "DC voltage 2" },
  { MB_INPUTREG,   3025,  1,     30,     SDT_U16,   10,     "A",  "DC current 2" },
  { MB_INPUTREG,   3042,  1,     60,     SDT_U16,   10,    "°C",  "Inverter temperature" },
  { MB_INPUTREG,   3043,  1,    120,     SDT_U16,  100,    "Hz",  "Grid frequency" },
  { MB_INPUTREG,   3044,  1,     60,    SDT_APP2,    1,      "",  "Inverter status" },
  { MB_INPUTREG,   3054,  1,    300,    SDT_APP3,    1,      "",  "Country standard code" },
  { MB_INPUTREG,   3056,  1,     -1,     SDT_S32,    1,   "Var",  "Reactive power" },
  { MB_INPUTREG,   3061,  1,    120,   SDT_SNHEX,    1,      "",  "Inverter SN" },
  { MB_INPUTREG,   3072,  1,     60,    SDT_APP6,    1,      "",  "Working status" },
  { MB_INPUTREG,   3076,  1,    300,     SDT_U16,    1,     "h",  "System Time (hour)" },
  { MB_INPUTREG,   3077,  1,    300,     SDT_U16,    1,     "m",  "System Time (min)" },
  { MB_INPUTREG,   3084,  1,     -1,     SDT_S32,    1,     "W",  "Meter active power" },
  {}
};

/* RS485_MODBUS (ESINV-33000ID) energy storage inverter protocol */
solisreg solisESINV[] = {
  { MB_INPUTREG,  33000,  0,    300,    SDT_APP1,    1,      "",  "Model no" },
  { MB_INPUTREG,  33004,  0,    120,   SDT_SNASC,    1,      "",  "Inverter SN" },
  { MB_INPUTREG,  33035,  0,     60,     SDT_U16,   10,   "kWh",  "Today energy generation" },
  { MB_INPUTREG,  33036,  0,     60,     SDT_U16,   10,   "kWh",  "Yesterday energy generation" },
  { MB_INPUTREG,  33049,  0,     30,     SDT_U16,   10,     "V",  "DC voltage 1" },
  { MB_INPUTREG,  33050,  0,     30,     SDT_U16,   10,     "A",  "DC current 1" },
  { MB_INPUTREG,  33051,  0,     30,     SDT_U16,   10,     "V",  "DC voltage 2" },
  { MB_INPUTREG,  33052,  0,     30,     SDT_U16,   10,     "A",  "DC current 2" },
  { MB_INPUTREG,  33053,  0,     30,     SDT_U16,   10,     "V",  "DC voltage 3" },
  { MB_INPUTREG,  33054,  0,     30,     SDT_U16,   10,     "A",  "DC current 3" },
  { MB_INPUTREG,  33055,  0,     30,     SDT_U16,   10,     "V",  "DC voltage 4" },
  { MB_INPUTREG,  33056,  0,     30,     SDT_U16,   10,     "A",  "DC current 4" },
  { MB_INPUTREG,  33057,  0,     60,     SDT_U32,    1,     "W",  "Total DC output power" },
  { MB_INPUTREG,  33071,  0,     60,     SDT_U16,   10,     "V",  "DC bus voltage" },
  { MB_INPUTREG,  33093,  0,     60,     SDT_U16,   10,    "°C",  "Inverter temperature" },
  { MB_INPUTREG,  33094,  0,    120,     SDT_U16,  100,    "Hz",  "Grid frequency" },
  { MB_INPUTREG,  33133,  0,     60,     SDT_U16,   10,     "V",  "Battery voltage" },
  { MB_INPUTREG,  33134,  0,     60,     SDT_S16,   10,     "A",  "Battery current" },
  { MB_INPUTREG,  33139,  0,     60,     SDT_U16,    1,     "%",  "Battery capacity SOC" },
  { MB_INPUTREG,  33140,  0,     60,     SDT_U16,    1,     "%",  "Battery health SOH" },
  { MB_INPUTREG,  33141,  0,     60,     SDT_U16,  100,     "V",  "Battery voltage BMS" },
  { MB_INPUTREG,  33142,  0,     60,     SDT_S16,   10,     "A",  "Battery current BMS" },
  { MB_INPUTREG,  33147,  0,     60,     SDT_U16,    1,     "W",  "Household load power" },
  { MB_INPUTREG,  33149,  0,     60,     SDT_S32,    1,     "W",  "Battery power" },
  { MB_INPUTREG,  33163,  0,     60,     SDT_U16,   10,   "kWh",  "Today battery charge energy" },
  { MB_INPUTREG,  33164,  0,    300,     SDT_U16,   10,   "kWh",  "Yesterday battery charge energy" },
  { MB_INPUTREG,  33171,  0,     60,     SDT_U16,   10,   "kWh",  "Today energy imported from grid" },
  { MB_INPUTREG,  33172,  0,    300,     SDT_U16,   10,   "kWh",  "Yesterday energy imported from grid" },
  { MB_INPUTREG,  33179,  0,     60,     SDT_U16,   10,   "kWh",  "Today load energy consumption" },
  { MB_INPUTREG,  33180,  0,    300,     SDT_U16,   10,   "kWh",  "Yesterday load energy consumption" },
  {}
};

InfluxDBClient influxclient(INFLUXDB_URL, INFLUXDB_DB_NAME);
SoftwareSerial modbusSerial(MODBUSPINRX, MODBUSPINTX);
ModbusMaster modbus;

unsigned long readlast[(sizeof(solisINV) > sizeof(solisESINV) ? sizeof(solisINV) : sizeof(solisESINV) ) / sizeof(solisreg)];
char serialnumber[4 * SNHEXWORDS + 1];
int serialvalid = 0;
solisreg *solis = solisUNKNOWN;

void setup() {
  Serial.begin(115200);

  wifi_station_set_hostname(WIFINAME);
  WiFiManager wifimanager;
  wifimanager.autoConnect(WIFINAME);

  modbusSerial.begin(MODBUSBAUD);
  modbus.begin(MODBUSINVERTERID, modbusSerial);

  Serial.println("solis2influx started");
}

void loop() {

  Point influxdb(INFLUXDB_MEAS);

  for (int i = 0; solis[i].modbusaddr; i++) {

    if (!( millis() - readlast[i] >= (1000 * solis[i].readdelay) ) || (solis[i].readdelay < 0)) {
      continue;
    }
    readlast[i] = millis();

    unsigned int mbreqaddr = solis[i].modbusaddr - solis[i].modbusoffset;
    unsigned int mbreqlen;
    unsigned int mbreadresult;
    unsigned int datadiv = ( solis[i].datadiv > 1 ) ? solis[i].datadiv : 0;

    Serial.print(solis[i].dataname);
    Serial.print(" = ");

    switch (solis[i].datatype) {
      case SDT_SNHEX:
        mbreqlen = SNHEXWORDS;
        break;
      case SDT_SNASC:
        mbreqlen = SNASCWORDS;
        break;
      case SDT_U32:
      case SDT_S32:
        mbreqlen = 2;
        break;
      default:
        mbreqlen = 1;
        break;
    }

    switch (solis[i].modbusobject) {
      case MB_COIL:
        mbreadresult = modbus.readCoils(mbreqaddr, mbreqlen);
        break;
      case MB_DISCRETEINPUT:
        mbreadresult = modbus.readDiscreteInputs(mbreqaddr, mbreqlen);
        break;
      case MB_INPUTREG:
        mbreadresult = modbus.readInputRegisters(mbreqaddr, mbreqlen);
        break;
      case MB_HOLDINGREG:
        mbreadresult = modbus.readHoldingRegisters(mbreqaddr, mbreqlen);
        break;
    }

    if (mbreadresult != modbus.ku8MBSuccess) {
      Serial.println("Error: Modbus read failure");
      readlast[i] += 1000 * 60;
      delay(500);
      continue;
    }

    switch (solis[i].datatype) {
      case SDT_U16:
        if (datadiv) {
          float regvalue = modbus.getResponseBuffer(0) / (float) datadiv;
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        } else {
          unsigned int regvalue = modbus.getResponseBuffer(0);
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        }
        break;

      case SDT_U32:
        if (datadiv) {
          float regvalue = ((modbus.getResponseBuffer(0) << 16) | modbus.getResponseBuffer(1)) / (float) datadiv;
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        } else {
          unsigned long regvalue = (unsigned long)(modbus.getResponseBuffer(0) << 16) | modbus.getResponseBuffer(1);
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        }
        break;

      case SDT_S16:
        if (datadiv) {
          float regvalue = (int) modbus.getResponseBuffer(0) / (float) datadiv;
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        } else {
          int regvalue = (int) modbus.getResponseBuffer(0);
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        }
        break;

      case SDT_S32:
        if (datadiv) {
          float regvalue = (((long)(modbus.getResponseBuffer(0) << 16)) | modbus.getResponseBuffer(1)) / (float) datadiv;
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        } else {
          long regvalue = (long)(modbus.getResponseBuffer(0) << 16) | modbus.getResponseBuffer(1);
          Serial.print(regvalue);
          influxdb.addField(solis[i].dataname, regvalue);
        }
        break;

      case SDT_ITYPE:  /* Solis inverter type definition, switches lookup table */
        {
          char buf[2 + 4 + 1];
          unsigned int regvalue = modbus.getResponseBuffer(0);
          sprintf(buf, "0x%04X", regvalue);
          Serial.print(buf);
          if ((regvalue / 100) == 10) {           /* RS485_MODBUS (INV-3000ID EPM-36000ID) inverter protocol */
            solis = solisINV;
          } else if ((regvalue / 100) == 20) {    /* RS485_MODBUS (ESINV-33000ID) energy storage inverter protocol */
            solis = solisESINV;
          }
        }
        break;

      case SDT_SNHEX:  /* 16 characters serial number, solis-style hex encoded */
        for (int j = 0; j < SNHEXWORDS; j++) {
          unsigned int r = modbus.getResponseBuffer(j);
          sprintf(serialnumber + (j * 4), "%02x%02x",
                  (r & 0x0F) << 4 | (r & 0xF0) >> 4, (r & 0x0F00) >> 4 | (r & 0xF000) >> 12);
        }
        serialnumber[SNHEXWORDS * 4] = 0;
        serialvalid = 1;
        Serial.print(serialnumber);
        break;

      case SDT_SNASC:  /* Serial number, solis-style ASCII encoded */
        for (int j = 0; j < SNASCWORDS; j++) {
          unsigned int r = modbus.getResponseBuffer(j);
          serialnumber[j] = r & 0x0F;
        }
        serialnumber[SNASCWORDS] = 0;
        /* serialvalid = 1;   decoding logic above not tested, docs are confusing, please help */
        Serial.print(serialnumber);
        break;

      case SDT_APP6:  /* Working status */
        {
          char *wstatus[] = {
            "Normal", "Initializing", "Grid off", "Fault to stop", "Standby", "Derating",
            "Limitating", "Backup OV Load", "Grid Surge Warn", "Fan fault Warn", "Reserved",
            "AC SPD ERROR VgSpdFail", "DC SPD ERROR DcSpdFail", "Reserved", "Reserved", "Reserved"
          };
          char buf[64];
          unsigned int regvalue = modbus.getResponseBuffer(0);
          for (int j = 0; j < 16; j++) {
            sprintf(buf, "%s - %s", solis[i].dataname, wstatus[j]);
            influxdb.addField(buf, (bool)(regvalue & (1 << j)));

            if (regvalue & (1 << j)) {
              Serial.print(wstatus[j]);
              Serial.print(" ");
            }
          }
        }
        break;

      default:  /* Hex print */
        {
          char buf[2 + 4 + 1];
          unsigned int regvalue = modbus.getResponseBuffer(0);
          sprintf(buf, "0x%04X", regvalue);
          Serial.print(buf);
        }
        break;
    }

    Serial.println(solis[i].dataunit);

  }

#ifdef INFLUXDB_URL
  if (serialvalid && influxdb.hasFields()) {
    influxdb.addTag("serialnumber", serialnumber);

    Serial.print("Writing to influxDB: ");
    Serial.println(influxclient.pointToLineProtocol(influxdb));

    if (!influxclient.writePoint(influxdb)) {
      Serial.print("InfluxDB write failed: ");
      Serial.println(influxclient.getLastErrorMessage());
    }
  }
#endif

  delay(90);
}
