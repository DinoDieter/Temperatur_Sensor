/* EEPROM Aufteilung für Konfigurationsdaten
Position     Länge    Wert
0            1        Firststart Flag
1            1        Updateintervall
2            1        Connectiontimeout
3            1        Flag für zu viele Connection Timeouts -> SETUP Modus wird aktiviert bei 3 aufeinanderfolgenden Timeouts
4            1        IP Typ 0=DHCP, 1=Statisch
5            4        IP-Adresse
9            4        Gateway IP-Adresse
13           4        Subnet Mask
17           4        DNS-Server IP-Adresse
18-20        3        Frei
21-52        32       WLAN SSID
53-116       64       WLAN PWD
117-148      32       SENSORSSID
149-212      64       SENSORPWD
213-237      25       Standort
238          1        Frei
239-255      16       Thingspeak API-Key
256-271      16       Pushingbox Scenario Device-ID
*/

/* RTC-Ram Aufteilung für Flags
Position     Wert
0            Startmodus Flag
1            Anzahl Wakeups
*/

#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <DHT.h>
#include "config/all.h"

extern "C" {
  #include "user_interface.h"
}

#define RESETPIN 2             
#define DHTPIN 4            
#define LEDPIN 5             
#define DHTPOWERPIN 3
#define DHTTYPE DHT22        
#define RTC_BASE 28
#define RTC_RAM_ARRAY_SIZE 2

ADC_MODE(ADC_VCC);                                  //Der ADC wird so konfiguriert das er die Systemspannung misst, es darf nichts am ADC (TOUT) Pin angechlossen sein!

const String SOFTWAREVERSION="2.020316.F";          //Version der Software (X.YYYYYY.Z X = Build des Tages, Y = Datum, Z = Typ (Z=Zwischenversion, F=Finale Version))

//Variablen werden mit EEPROM Konfiguration überschrieben wenn FirstStartFlag im EEPROM = 111 (bedeutet Sensor ist konfiguriert) ist!
byte FIRSTSTARTFLAG=0;                              //Flag zur Erkennung ob System im Setup Modus starten muss weil es keine Konfiguration im EEPROM hat
byte UPDATEINTERVAL=15;                             //Aktualisierungsinterval für den Thingspeak Channel in Minuten (Mindestens 15 lt. Thingspeak gewünscht ;) )
byte CONNECTIONTIMEOUT=60;                          //Verbindungstimeout für die Verbindung mit unserem WLAN (Sekunden)
byte STATICIP=0;                                    //Art der IP-Adresszuteilung 0=IP per DHCP, 1=Statische IP
String THINGSPEAKAPIKEY="KU2N6AA4VK2QL15Z";         //Write Key des Thingspeak Channels
String PUSHINGBOXID="v0123456789ABCDE";                       //Device-ID des Pushingbox Scenarios
String WLANSSID="";                                 //Die SSID eures WLAN kommt hier rein
String WLANPWD="";                                  //Das WLAN Passwort eures WLAN kommt hier rein
String SENSORSSID="ESP8266-DHT22-SENSOR";           //Die Standard SSID für unser Sensor WLAN
String SENSORPWD="";                                //Das Passwort für unser Sensor WLAN
String SENSORSTANDORT="";                           //Standort des Sensors (Max 25 Zeichen)
IPAddress ip(0,0,0,0);                              //Sensor Server IP-Adresse bei statischer Konfiguration
IPAddress gw(0,0,0,0);                              //Gateway IP-Adresse bei statischer Konfiguration
IPAddress sub(255,255,255,0);                       //Subnet Mask bei statischer Konfiguration
IPAddress dns(0,0,0,0);                             //DNS Server IP-Adresse bei statischer Konfiguration

//Konstanten definieren, diese Werte können nur im Programm angepasst werden

const int SHUTDOWNVCC=2900;                         //Systemabschaltschwelle in Volt ohne Dezimalpunkt 4 Stellen (z.B. 3,1 V = 3100)
const int WARNVCC=3290;                             //Warnschwelle in Volt ohne Dezimalpunkt 4 Stellen (z.B. 3,29 V = 3290) ab der eine Pushingbox Warnamil versendet wird
const int MAXSENSORERRORCOUNT=5;                    //Maximale Anzahl Versuche vom Sensor zu lesen bevor das System resettet wird
const int MAXCONNECTIONTRIES=5;                     //Maximale Anzahl an WLAN Verbindungsversuchen bevor wieder in den Setup Modus gebootet wird
const int DNSPORT = 53;                             //Der Port unseres DNS-Servers (NICHT ÄNDERN)

//Vorgaben für das automatische Update, diese Daten können für einen eigenen Updateserver angepasst werden
const char* UPDATESERVERFILE=".php"; //(NICHT ÄNDERN)
const char* UPDATESERVER="192.168.4.34";     //(NICHT ÄNDERN)
const int UPDATESERVERPORT=80;                      //(NICHT ÄNDERN)

//Sonstige benötigte Variablen definieren 
String HTMLFORMULARWLAN="";                         //Hilfsvariable für das Formular zur Auswahl des WLAN
float AKTUELLETEMPERATUR=0;                         //Variable für die aktuelle Temperatur
float AKTUELLELUFTFEUCHTE=0;                        //Variable für die aktuelle Luftfeuchte
float KORREKTURTEMPERATUR=0;                        //Korrekturwert für die Temperatur
float KORREKTURLUFTFEUCHTE=0;                       //Korrekturwert für die Luftfeuchte
byte TIMEOUTFLAG=0;                                 //Hilfsvariable zum zählen der Verbindungstimeouts
unsigned char RAMFlags[2];                          //Hilfsarray für die RTC-Ram Daten
unsigned char ANZAHLWAKEUPS=0;                      //Anzahl aktuell durchgeführter Wakeupstarts
int step=0;                                         //Variable für den Setup-Wizzard

DHT dht(DHTPIN, DHTTYPE);
WiFiServer server(80);
DNSServer dnsServer;

// Daten der auf den Webseiten genutzen Bilder (Logo.png und Favicon)

    
void GetSensorData() {
  digitalWrite(LEDPIN, 1);
  delay(50);
  digitalWrite(LEDPIN, 0);

  int SENSORERRORCOUNT=0;

  AKTUELLELUFTFEUCHTE = dht.readHumidity();
  AKTUELLETEMPERATUR = dht.readTemperature();

  while (isnan(AKTUELLELUFTFEUCHTE) || isnan(AKTUELLETEMPERATUR)) {
    SENSORERRORCOUNT+=1;
    
    digitalWrite(LEDPIN, 1);
    delay(50);
    digitalWrite(LEDPIN, 0);
    
    delay(2450);
    
    AKTUELLELUFTFEUCHTE = dht.readHumidity();
    AKTUELLETEMPERATUR = dht.readTemperature();

    if (SENSORERRORCOUNT >= MAXSENSORERRORCOUNT) {
      Serial.println(F("Zuviele Sensorfehler in Folge, starte Sensor neu..."));
      Serial.println(F(""));
      
      WiFi.disconnect();

      delay (100);
      
      ESP.deepSleep(1, WAKE_RF_DEFAULT);
      delay(100);
    }
  }
  
  AKTUELLELUFTFEUCHTE += KORREKTURLUFTFEUCHTE;
  AKTUELLETEMPERATUR += KORREKTURTEMPERATUR;
}

void ShowSensorData() {
  Serial.print(F("Luftfeuchte: "));
  Serial.print(AKTUELLELUFTFEUCHTE, 1);
  Serial.println(F(" %"));
  Serial.print(F("Temperatur: "));
  Serial.print(AKTUELLETEMPERATUR, 1);
  Serial.println(F(" *C"));
  Serial.println(F(""));
}

void ReadEEPROMConfig() {
  EEPROM.begin(512);

  EEPROM.get(0,FIRSTSTARTFLAG);
  
  Serial.print(F("FIRSTSTARTFLAG="));
  Serial.println(FIRSTSTARTFLAG);
  
  EEPROM.get(1,UPDATEINTERVAL);

  Serial.print(F("UPDATEINTERVAL="));
  Serial.println(UPDATEINTERVAL);

  EEPROM.get(2,CONNECTIONTIMEOUT);

  Serial.print(F("CONNECTIONTIMEOUT="));
  Serial.println(CONNECTIONTIMEOUT);

  EEPROM.get(3,TIMEOUTFLAG);

  Serial.print(F("TIMEOUTFLAG="));
  Serial.println(TIMEOUTFLAG);

  EEPROM.get(4,STATICIP);

  Serial.print(F("STATICIP="));
  Serial.println(STATICIP);

  ip[0]=EEPROM.read(5);
  ip[1]=EEPROM.read(6);
  ip[2]=EEPROM.read(7);
  ip[3]=EEPROM.read(8);
  
  gw[0]=EEPROM.read(9);
  gw[1]=EEPROM.read(10);
  gw[2]=EEPROM.read(11);
  gw[3]=EEPROM.read(12);
  
  sub[0]=EEPROM.read(13);
  sub[1]=EEPROM.read(14);
  sub[2]=EEPROM.read(15);
  sub[3]=EEPROM.read(16);

  dns[0]=EEPROM.read(17);
  dns[1]=EEPROM.read(18);
  dns[2]=EEPROM.read(19);
  dns[3]=EEPROM.read(20);

  Serial.print(F("IP="));
  Serial.println(ip);
  
  Serial.print(F("GATEWAY="));
  Serial.println(gw);
  
  Serial.print(F("SUBNET="));
  Serial.println(sub);
  
  Serial.print(F("DNS="));
  Serial.println(dns);
  
  WLANSSID="";
  
  for (int i = 21; i < 53; ++i)
  {
    WLANSSID += char(EEPROM.read(i));
  }

  Serial.print(F("WLANSSID="));
  Serial.println(WLANSSID.c_str());

  WLANPWD="";
  
  for (int i = 53; i < 117; ++i)
  {
    WLANPWD += char(EEPROM.read(i));
  }
  
  Serial.print(F("WLANPWD="));
  Serial.println(WLANPWD.c_str());

  SENSORSSID="";

  for (int i = 117; i < 149; ++i)
  {
    SENSORSSID += char(EEPROM.read(i));
  }
  
  Serial.print(F("SENSORSSID="));
  Serial.println(SENSORSSID.c_str());

  SENSORPWD="";

  for (int i = 149; i < 213; ++i)
  {
    SENSORPWD += char(EEPROM.read(i));
  }

  Serial.print(F("SENSORPWD="));
  Serial.println(SENSORPWD.c_str());

  SENSORSTANDORT="";

  for (int i = 213; i < 238; ++i)
  {
    SENSORSTANDORT += char(EEPROM.read(i));
  }

  Serial.print(F("SENSORSTANDORT="));
  Serial.println(SENSORSTANDORT.c_str());

  THINGSPEAKAPIKEY="";

  for (int i = 239; i < 256; ++i)
  {
    THINGSPEAKAPIKEY += char(EEPROM.read(i));
  }

  Serial.print(F("THINGSPEAKAPIKEY="));
  Serial.println(THINGSPEAKAPIKEY.c_str());

  PUSHINGBOXID="";

  for (int i = 256; i < 272; ++i)
  {
    PUSHINGBOXID += char(EEPROM.read(i));
  }

  Serial.print(F("PUSHINGBOXID="));
  Serial.println(PUSHINGBOXID.c_str());

  EEPROM.end();
}

void SendSensorData() {
  analogWrite(LEDPIN, 100);
  
  Serial.print(F("Verbinde mit "));
  Serial.println(THINGSPEAKHOST);

  WiFiClient client;

  delay(2000);

  if (!client.connect(THINGSPEAKHOST, THINGSPEAKPORT)) {
    Serial.println(F(""));
    Serial.println(F("Verbindung mit Thingspeak-Server fehlgeschlagen!"));
  } else {
    Serial.println(F("Verbindung mit Thingspeak-Server erfolgreich hergestellt"));
    Serial.println(F("Sende Daten an Thingspeak Channel..."));

    String POSTString ="api_key=";
    POSTString += THINGSPEAKAPIKEY.c_str();
    POSTString += "&field1=";
    POSTString += AKTUELLETEMPERATUR;
    POSTString += "&field2=";
    POSTString += AKTUELLELUFTFEUCHTE;
    POSTString += "&field3=";
    POSTString += String(ESP.getVcc()).substring(0,1);
    POSTString += ".";
    POSTString += String(ESP.getVcc()).substring(1);

    client.print(F("POST /update HTTP/1.1\n")); 
    client.print(F("HOST: "));
    client.print(THINGSPEAKHOST);
    client.print(F("\n"));
    client.print(F("X-THINGSPEAKAPIKEY:"));
    client.print(THINGSPEAKAPIKEY.c_str());
    client.print(F("\n"));
    client.print(F("Connection: close\n")); 
    client.print(F("Content-Type: application/x-www-form-urlencoded\n")); 
    client.print(F("Content-Length: "));
    client.print(POSTString.length()); 
    client.print(F("\n\n"));
    client.print(POSTString);

    delay(1000);
        
    client.stop();
    
    Serial.println(F("Verbindung zum Server beendet"));
    Serial.println(F("Daten an Thingspeak Channel gesendet"));
  }  

  analogWrite(LEDPIN, 0);
}

void SendAkkuWarnMail() {
  analogWrite(LEDPIN, 25);
  
  WiFiClient client;

  Serial.print(F("Verbinde mit "));
  Serial.print(PUSHINGBOXSERVER);
  Serial.println(F(" ..."));

  if (client.connect(PUSHINGBOXSERVER, PUSHINGBOXPORT)) { 
    Serial.println(F("Verbindung erfolgreich hergestellt..."));
    Serial.println(F(""));
    
    String getStr = "/pushingbox?devid=";
    getStr += PUSHINGBOXID.c_str();

    getStr += "&STANDORT=";
    getStr += SENSORSTANDORT.c_str();
    
    client.print("GET " + getStr + " HTTP/1.1\r\n"); 
    client.print("HOST: ");
    client.print(PUSHINGBOXSERVER);
    client.print("\r\n\r\n"); 
    
    delay(100);
    
    client.stop();

    Serial.println(F("Akku Warnmail versendet!"));
    Serial.println(F(""));
    Serial.println(F("Verbindung abgebaut."));
  }  

  analogWrite(LEDPIN, 0);
}

void ShowSysInfo(int typ) {
  if (typ==1) {
    Serial.print(F("Software-Version: "));
    Serial.println(SOFTWAREVERSION);
    Serial.print(F("SDK-Version: "));
    Serial.println(ESP.getSdkVersion());
    Serial.print(F("ESP8266 Chip-ID: "));
    Serial.println(ESP.getChipId());
    Serial.print(F("ESP8266 Speed: "));  
    Serial.print(ESP.getCpuFreqMHz());
    Serial.println(F(" MHz"));
  }
  
  Serial.print(F("Free Heap Size: "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F(" Bytes"));
  Serial.print(F("Systemspannung: "));
  Serial.print(String(ESP.getVcc()).substring(0,1));
  Serial.print(F(","));
  Serial.print(String(ESP.getVcc()).substring(1));
  Serial.println(F(" Volt"));  
  Serial.println(F(""));
}

void ResetWiFi() {
  //RTC-Ram Flags für Setup Modus setzen und speichern
  RAMFlags[0]=0;     
  RAMFlags[1]=0;     
  
  while(system_rtc_mem_write(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
    delay(1);
  }

  //Firststartflag auf Setup Modus setzen
  EEPROM.begin(512);
  EEPROM.write(0,0);
  EEPROM.end();

  delay(100);

  WiFi.disconnect();

  delay(100);
  
  ESP.deepSleep(1,WAKE_RF_DEFAULT);
  delay(100);
}

void CheckForUpdate() {
    t_httpUpdate_return ret = ESPhttpUpdate.update(UPDATESERVER, UPDATESERVERPORT, UPDATESERVERFILE, SOFTWAREVERSION);

    switch(ret) {
        case HTTP_UPDATE_FAILED:
            Serial.println(F("UPDATE FEHLGESCHLAGEN!"));
            Serial.println(F(""));
            break;

        case HTTP_UPDATE_NO_UPDATES:
            Serial.println(F("KEIN UPDATE VORHANDEN!"));
            Serial.println(F(""));
            break;

        case HTTP_UPDATE_OK:
            Serial.println(F("UPDATE ERFOLGREICH!"));
            break;
    }
}

void setup() {
  pinMode(RESETPIN, INPUT_PULLUP);
  pinMode(LEDPIN, OUTPUT);
  pinMode(DHTPOWERPIN, OUTPUT);
    
  Serial.begin(115200);

  Serial.println(F(""));  
  Serial.println(F(""));  
  Serial.println(F("Starte den ESP8266-DHT22-SENSOR..."));
  Serial.println(F(""));

  Serial.println(F("Teste Systemspannung..."));
  Serial.println(F(""));

  if (ESP.getVcc() <= SHUTDOWNVCC) {
      Serial.println(F("!!! Systemspannung zu niedrig, schalte Sensor ab !!!"));
      Serial.println(F(""));

      delay(100);
      
      ESP.deepSleep(0,WAKE_RF_DEFAULT);
      delay(100);
  }
  
  Serial.print(String(ESP.getVcc()).substring(0,1));
  Serial.print(F(","));
  Serial.print(String(ESP.getVcc()).substring(1));
  Serial.println(F(" Volt ist OK"));
  Serial.println(F(""));
  
  Serial.println(F("Hole Konfiguration aus EEPROM..."));
  Serial.println(F(""));

  ReadEEPROMConfig();

  /*if (FIRSTSTARTFLAG == 111) {
    //Serial.println(F(""));
    //Serial.println(F("Gebe Zeit fuer Reset des ESP8266-DHT22-SENSOR..."));
    //Serial.println(F(""));
  
    unsigned long STARTZEITPUNKT=millis();
    
    //5 Sekunden die Möglichkeit geben den Sensor zu resetten
    //digitalWrite(LEDPIN, 1);

    while ((unsigned long)(millis()-STARTZEITPUNKT)<=5000) {
  
      yield();
          
      if (digitalRead(RESETPIN)==0) {
        digitalWrite(LEDPIN, 0);
        
        delay(500);
        
        while (digitalRead(RESETPIN)==0) {
          delay(100);
        }
      
        delay(500);
        
        ResetWiFi();
      }
    } 
  
   // digitalWrite(LEDPIN, 0);
  }
   */
  
  //Flags aus RTC-Ram einlesen
  while(system_rtc_mem_read(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
    delay(1);
  }

  Serial.println(F("Ist der Sensor Konfiguriert?"));
  Serial.println(F(""));

  if (FIRSTSTARTFLAG == 111) {
    Serial.println(F("Sensor ist konfiguriert..."));
    Serial.println(F(""));

    Serial.println(F("Teste Startflag im RTC-Ram..."));
    Serial.println(F(""));

    if (RAMFlags[0] == 0) {
      Serial.println(F("Keine Datenuebertragung notwendig..."));
      Serial.println(F(""));
  
      ANZAHLWAKEUPS=RAMFlags[1];
      ANZAHLWAKEUPS+=1;
      
      Serial.print(F("Sensor ist "));
      Serial.print(ANZAHLWAKEUPS);
      Serial.println(F(" mal aufgewacht."));
      Serial.println(F(""));

      if(ANZAHLWAKEUPS>=UPDATEINTERVAL) {
        RAMFlags[0]=1;
        RAMFlags[1]=1;

        Serial.println(F("Sensor wird neu gestartet im Uebertragungs-Modus..."));
        Serial.println(F(""));
        
        while(system_rtc_mem_write(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
          delay(1);
        }
  
        ESP.deepSleep(1,WAKE_RF_DEFAULT);
        delay(100);
      } 
      
      else {
    
        Serial.println(F("Gehe schlafen..."));
        Serial.println(F(""));

        RAMFlags[0]=0;
        RAMFlags[1]=ANZAHLWAKEUPS;
        
        while(system_rtc_mem_write(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
          delay(1);
        }
  
        ESP.deepSleep(52500000-millis(), WAKE_RF_DEFAULT);
        delay(100);
      }
    }
  }

  Serial.println(F("Starte im Uebertragungs-Modus..."));
  Serial.println(F(""));

  ShowSysInfo(1);

  float TEMPTEMPERATUR1=0;
  float TEMPLUFTFEUCHTE1=0;
  float TEMPTEMPERATUR2=0;
  float TEMPLUFTFEUCHTE2=0;
  
  if (FIRSTSTARTFLAG == 112) {
    digitalWrite(DHTPOWERPIN, 1);
  
    Serial.println(F("Sensorkalibrierung gestartet..."));
    Serial.println(F(""));
  
    for (int i=1; i<=5; i++) {
      delay(2000);
      
      GetSensorData();
      
      ShowSensorData();
  
      TEMPTEMPERATUR1+=AKTUELLETEMPERATUR;
      TEMPLUFTFEUCHTE1+=AKTUELLELUFTFEUCHTE;  
    }
  
    digitalWrite(DHTPOWERPIN, 0);

    TEMPTEMPERATUR1/=5;
    TEMPLUFTFEUCHTE1/=5;
  
    Serial.print(F("Durchschnittliche Luftfeuchte: "));
    Serial.print(TEMPLUFTFEUCHTE1, 1);
    Serial.println(F(" %"));
    Serial.print(F("Durchschnittliche Temperatur: "));
    Serial.print(TEMPTEMPERATUR1, 1);
    Serial.println(F(" *C"));
   
  }
  
  Serial.println(F(""));
  Serial.println(F("WLAN-Netzwerk-Scan gestartet..."));
      
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  int n = WiFi.scanNetworks();
    
  Serial.println(F("WLAN-Netzwerk-Scan abgeschlossen..."));
  Serial.println(F(""));

  if (n == 0) {
    Serial.println(F("Kein WLAN-Netzwerk gefunden!"));
  } else {
    Serial.print(n);

    if(n<=1) {
     Serial.println(F(" WLAN-Netzwerk gefunden"));
    } else {
      Serial.println(F(" WLAN-Netzwerke gefunden"));
    }
  
    Serial.println(F(""));

    for (int i = 0; i < n; ++i) { 
      Serial.print(i + 1);
      Serial.print(F(": "));
      Serial.print(WiFi.SSID(i));
      Serial.print(F(" ("));
      Serial.print(WiFi.RSSI(i));
      Serial.print(F(")"));
      Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*");
    }
  }

  HTMLFORMULARWLAN = "<b><u>WLAN-Netzwerk ausw&auml;hlen:</u></b><br /><br />";

  for (int i = 0; i < n; ++i) {
    // Erstelle Webseite mit allen SSID und RSSI Werten der gefundenen Netzwerke
    HTMLFORMULARWLAN += "<input type='radio' name='WLANSSID' value='";
    HTMLFORMULARWLAN += WiFi.SSID(i);
    HTMLFORMULARWLAN += "'";

    if (WiFi.SSID(i)==WLANSSID.substring(0, WiFi.SSID(i).length()+1)) {
      HTMLFORMULARWLAN += " checked";
    } 

    HTMLFORMULARWLAN += ">";
    HTMLFORMULARWLAN += WiFi.SSID(i);
    HTMLFORMULARWLAN += " (";
    HTMLFORMULARWLAN += WiFi.RSSI(i);
    HTMLFORMULARWLAN += ")";
    HTMLFORMULARWLAN += (WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*";
    HTMLFORMULARWLAN += "<br />";
  }
  
  Serial.println(F("")); 

  if (FIRSTSTARTFLAG == 111) {
    dht.begin();
  
    Serial.print(F("Verbinde mit WLAN "));
    Serial.println(WLANSSID);
  
    if (STATICIP==1) {
      WiFi.config(ip, gw, sub, dns);
    }

    WiFi.begin(WLANSSID.c_str(), WLANPWD.c_str());
    
    int WLANTIMEOUT=0;
    
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.print(F("."));
      WLANTIMEOUT+=1;
  
      if (WLANTIMEOUT>=CONNECTIONTIMEOUT) {
        Serial.println(F(""));
        Serial.println(F("Timeout!"));  
        
        TIMEOUTFLAG+=1;

        if(TIMEOUTFLAG>=MAXCONNECTIONTRIES) {
          ResetWiFi();
        } else {
          EEPROM.begin(512);
          EEPROM.write(3, TIMEOUTFLAG);
          EEPROM.end();

          delay(100);
          
          WiFi.disconnect();

          delay(100);
          
          ESP.deepSleep(1000000,WAKE_RF_DEFAULT);
          delay(100);
        }
      }
    }
  
    EEPROM.begin(512);
    EEPROM.write(3, 0);
    EEPROM.end();

    Serial.println(F(""));
    Serial.println(F("Verbindung mit WLAN hergestellt"));  
    Serial.println(F(""));
    Serial.print(F("IP-Addresse: "));
    Serial.println(WiFi.localIP());
    Serial.println(F(""));
    Serial.println(F("Update-Check gestartet..."));
    
    //CheckForUpdate();

    Serial.println(F("Setze Sensorkalibrierung fort... "));
    Serial.println(F(""));

    digitalWrite(DHTPOWERPIN, 1);
   
    for (int i=1; i<=5; i++) {
      delay(2000);

      GetSensorData();

      ShowSensorData();
      
      TEMPTEMPERATUR2+=AKTUELLETEMPERATUR;
      TEMPLUFTFEUCHTE2+=AKTUELLELUFTFEUCHTE;  
    }
  
    digitalWrite(DHTPOWERPIN, 0);

    TEMPTEMPERATUR2/=5;
    TEMPLUFTFEUCHTE2/=5;
  
    Serial.print(F("Durchschnittliche Luftfeuchte: "));
    Serial.print(TEMPLUFTFEUCHTE2, 1);
    Serial.println(F(" %"));
    Serial.print(F("Durchschnittliche Temperatur: "));
    Serial.print(TEMPTEMPERATUR2, 1);
    Serial.println(F(" *C"));
    Serial.println(F(""));
  
    KORREKTURTEMPERATUR=TEMPTEMPERATUR1-TEMPTEMPERATUR2;
    KORREKTURLUFTFEUCHTE=TEMPLUFTFEUCHTE1-TEMPLUFTFEUCHTE2;
    
    Serial.print(F("Luftfeuchte Korrekturwert: "));
    Serial.println(KORREKTURLUFTFEUCHTE, 2);
    Serial.print(F("Temperatur Korrekturwert: "));
    Serial.println(KORREKTURTEMPERATUR, 2);
    Serial.println(F(""));
  } else {
    Serial.println(F("Sensor ist nicht konfiguriert..."));
    Serial.println(F(""));

    digitalWrite(LEDPIN ,1);
    
    String SENSORSSIDSETUP = "ESP8266-DHT22-DS-SENSOR-SETUP";
    SENSORPWD = "";
    FIRSTSTARTFLAG=1;
    
    Serial.println(F("Starte im Setup-Modus!!!"));
    WiFi.softAP(SENSORSSIDSETUP.c_str(), SENSORPWD.c_str());
  
    delay(100);
    
    Serial.println(F(""));
    Serial.println(F("AP Modus gestartet"));
    Serial.print(F("SSID: "));
    Serial.println(SENSORSSIDSETUP.c_str());
    Serial.print(F("AP IP-Adresse: "));
    Serial.println(WiFi.softAPIP());
    
    Serial.println(F(""));
    Serial.println(F("Starte DNS-Server"));
    
    dnsServer.start(DNSPORT, "*", WiFi.softAPIP());
  
    Serial.println(F("DNS-Server gestartet")); 
    Serial.println(F(""));
    Serial.println(F("Starte HTTP-Server"));
  
    server.begin();
  
    Serial.println(F("HTTP-Server gestartet")); 
    Serial.println(F(""));
  } 

  Serial.println(F("...ESP8266-DHT22-SENSOR erfolgreich gestartet."));
  Serial.println(F(""));  
}

void loop() {    
  if (FIRSTSTARTFLAG == 111) {
    if (ESP.getVcc() <= WARNVCC && PUSHINGBOXID.length() == 16) {
      SendAkkuWarnMail();
    }
    
    digitalWrite(DHTPOWERPIN, 1);

    delay(2000);
    
    GetSensorData();

    //Sensor direkt wieder abschalten um Strom zu sparen
    digitalWrite(DHTPOWERPIN, 0);

    //Daten an Thingspeak senden
    SendSensorData();

    //RTC-Ram Flags neu setzen und speichern
    RAMFlags[0]=0;       //Startmodus auf Wakeup stellen
    RAMFlags[1]=0;       //Anzahl Wakeups auf 0 setzen
    
    while(system_rtc_mem_write(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
      delay(1);
    }

    WiFi.disconnect();
    
    delay(100);

    //Schlafen gehen
    Serial.println(F(""));
    Serial.println(F("Gehe schlafen..."));
    Serial.println(F(""));

    ESP.deepSleep(60000000-millis(), WAKE_RF_DEFAULT);
    delay(100);
  }
  
  dnsServer.processNextRequest();

  WiFiClient client = server.available();

  if(client) {
    while(client.connected() && !client.available()){
      delay(1);
    }

    String RequestString = client.readStringUntil('\r');

    int addr_start = RequestString.indexOf(' ');
    int addr_end = RequestString.indexOf(' ', addr_start + 1);

    if (addr_start == -1 || addr_end == -1) {
      Serial.print(F("Ungueltige Anfrage: "));
      Serial.println(RequestString);
      Serial.println(F(""));
      
      client.stop();
    } else {
      RequestString = RequestString.substring(addr_start + 1, addr_end);

      Serial.print(F("Gueltige Anfrage: "));
      Serial.println(RequestString);
      Serial.println(F(""));
      
      String HTMLSite;
      String HTMLHeader;
      String ERRORTEXT;
      
      if ( RequestString.startsWith("/favicon.ico") ) 
      {
        HTMLHeader  = "HTTP/1.1 200 OK\r\n";
        HTMLHeader += "Content-Type: image/png\r\n";
        HTMLHeader += "Content-Length: ";
        HTMLHeader += sizeof(favicon);
        HTMLHeader += "\r\n";
        HTMLHeader += "Connection: close\r\n";
        HTMLHeader += "\r\n";
        
        client.print(HTMLHeader);
        delay(10);
        client.write(favicon,sizeof(favicon));
      }
      else if ( RequestString.startsWith("/logo.png")) 
      {
        HTMLHeader  = "HTTP/1.1 200 OK\r\n";
        HTMLHeader += "Content-Type: image/png\r\n";
        HTMLHeader += "Content-Length: ";
        HTMLHeader += sizeof(logo);
        HTMLHeader += "\r\n";
        HTMLHeader += "Connection: close\r\n";
        HTMLHeader += "\r\n";
        
        client.print(HTMLHeader);
        delay(10);
        client.write(logo,sizeof(logo));
      }

      if (RequestString.startsWith("/setwlanconfig?")) {
        step=2;
        
        //1. Wert extrahieren (WLAN SSID)
        int StartPosition=RequestString.indexOf('=');
        StartPosition++;
        
        int EndePosition=RequestString.indexOf("&");

        String NeueWLANSSID; 
        NeueWLANSSID = RequestString.substring(StartPosition,EndePosition);

        if (NeueWLANSSID.length()<1) {
          ERRORTEXT="Bitte WLAN SSID angeben!";
        }
        
        //2. Wert extrahieren (Netzwerk Passwort)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        String NeuesWLANPWD;
        NeuesWLANPWD = RequestString.substring(StartPosition,EndePosition);

        //3. Wert extrahieren (CONNECTIONTIMEOUT)
        int NeuesCONNECTIONTIMEOUT=0;
        NeuesCONNECTIONTIMEOUT=RequestString.substring(RequestString.lastIndexOf("=")+1).toInt();

        if(NeuesCONNECTIONTIMEOUT != 10 && NeuesCONNECTIONTIMEOUT != 30 && NeuesCONNECTIONTIMEOUT != 60) {
          ERRORTEXT="Falsches Connetion-Timeout!";
        }

        if(ERRORTEXT == "") {
          EEPROM.begin(512);
  
          delay(10);
  
          //Lösche alte Konfiguration
          for (int i=21; i<117; i++) {
            EEPROM.write(i ,0);
          }
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue WLANSSID ins EEPROM:"));
    
          for (int i = 0; i < NeueWLANSSID.length(); ++i){
            EEPROM.write(21+i, NeueWLANSSID[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeueWLANSSID[i]); 
          }    
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neues WLANPWD ins EEPROM:"));
  
          for (int i = 0; i < NeuesWLANPWD.length(); ++i){
            EEPROM.write(53+i, NeuesWLANPWD[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeuesWLANPWD[i]); 
          }    
  
          Serial.println(F(""));
          Serial.print(F("Schreibe neues CONNECTIONTIMEOUT ins EEPROM: "));
  
          Serial.print(F("schreibe: "));
          Serial.println(NeuesCONNECTIONTIMEOUT);
          
          EEPROM.write(2, NeuesCONNECTIONTIMEOUT);
  
          EEPROM.commit();
          EEPROM.end();
        } else {
          step=1;
        }
      }
      
      if (RequestString.startsWith("/setipconfig?")) {
        step=3;

        //1. Wert extrahieren (IP Typ)
        int StartPosition=RequestString.indexOf('=');
        StartPosition++;
        
        int EndePosition=RequestString.indexOf("&");

        int NeuerIPTYP; 
        NeuerIPTYP = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP != 0 && NeuerIPTYP != 1) {
          ERRORTEXT="Falsche IP-Adressart (DHCP oder statisch)!";
        }
        
        //2. Wert extrahieren (IP-Adresse Teil 1)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        int NeueIP1;
        NeueIP1 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueIP1 <1 || NeueIP1 >255) {
            ERRORTEXT="IP-Adresse Teil 1 ist falsch!";
          }
          
        } else if (NeueIP1 < 0 || NeueIP1 > 255) {
          ERRORTEXT="IP-Adresse Teil 1 ist falsch!";
        }
        
        //3. Wert extrahieren (IP-Adresse Teil 2)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueIP2;
        NeueIP2 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueIP2 < 0 || NeueIP2 > 255) {
          ERRORTEXT="IP-Adresse Teil 2 ist falsch!";
        }
        
        //4. Wert extrahieren (IP-Adresse Teil 3)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        int NeueIP3;
        NeueIP3 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueIP3 < 0 || NeueIP3 > 255) {
          ERRORTEXT="IP-Adresse Teil 3 ist falsch!";
        }
        
       //5. Wert extrahieren (IP-Adresse Teil 4)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueIP4;
        NeueIP4 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueIP4 < 0 || NeueIP4 > 255) {
          ERRORTEXT="IP-Adresse Teil 4 ist falsch!";
        }
        
        //6. Wert extrahieren (Gateway IP-Adresse Teil 1)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueGWIP1;
        NeueGWIP1 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueGWIP1 <1 || NeueGWIP1 >255) {
            ERRORTEXT="Gateway IP-Adresse Teil 1 ist falsch!";
          }
          
        } else if(NeueGWIP1 < 0 || NeueGWIP1 > 255) {
          ERRORTEXT="Gateway IP-Adresse Teil 1 ist falsch!";
        }
        
        //7. Wert extrahieren (Gateway IP-Adresse Teil 2)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueGWIP2;
        NeueGWIP2 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueGWIP2 < 0 || NeueGWIP2 > 255) {
          ERRORTEXT="Gateway IP-Adresse Teil 2 ist falsch!";
        }

        //8. Wert extrahieren (Gateway IP-Adresse Teil 3)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueGWIP3;
        NeueGWIP3 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueGWIP3 < 0 || NeueGWIP3 > 255) {
          ERRORTEXT="Gateway IP-Adresse Teil 3 ist falsch!";
        }

        //9. Wert extrahieren (Gateway IP-Adresse Teil 4)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueGWIP4;
        NeueGWIP4 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueGWIP4 < 0 || NeueGWIP4 > 255) {
          ERRORTEXT="Gateway IP-Adresse Teil 4 ist falsch!";
        }

        //10. Wert extrahieren (Subnet Mask Teil 1)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueMASK1;
        NeueMASK1 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueMASK1 != 255) {
            ERRORTEXT="Subnet Mask Teil 1 ist falsch (255)!";
          }
        } else if(NeueMASK1 < 0 || NeueMASK1 != 255) {
          NeueMASK1=0;
        }

        //11. Wert extrahieren (Subnet Mask Teil 2)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueMASK2;
        NeueMASK2 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueMASK2 !=0 && NeueMASK2 != 255) {
            ERRORTEXT="Subnet Mask Teil 2 ist falsch (0 oder 255)!";
          }
          
        } else if(NeueMASK2 < 0 || NeueMASK2 != 255) {
          NeueMASK2=0;
        }

        //12. Wert extrahieren (Subnet Mask Teil 3)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueMASK3;
        NeueMASK3 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueMASK3 !=0 && NeueMASK3 != 255) {
            ERRORTEXT="Subnet Mask Teil 3 ist falsch (0 oder 255)!";
          }
          
        } else if(NeueMASK3 < 0 || NeueMASK3 != 255) {
          NeueMASK3=0;
        }

        //13. Wert extrahieren (Subnet Mask Teil 4)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueMASK4;
        NeueMASK4 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuerIPTYP == 1) {
          if(NeueMASK4 !=0 && NeueMASK4 != 255) {
            ERRORTEXT="Subnet Mask Teil 4 ist falsch (0 oder 255)!";
          }
          
        } else if(NeueMASK4 < 0 || NeueMASK4 != 255) {
          NeueMASK4=0;
        }

        //14. Wert extrahieren (DNS Server IP-Adresse Teil 1)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        int NeueDNSIP1;
        NeueDNSIP1 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueDNSIP1 < 0 || NeueDNSIP1 > 255) {
          ERRORTEXT="DNS-Server IP-Adresse Teil 1 ist falsch!";
        }

        //15. Wert extrahieren (DNS Server IP-Adresse Teil 2)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        int NeueDNSIP2;
        NeueDNSIP2 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueDNSIP2 < 0 || NeueDNSIP2 > 255) {
          ERRORTEXT="DNS-Server IP-Adresse Teil 2 ist falsch!";
        }

        //16. Wert extrahieren (DNS Server IP-Adresse Teil 3)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        int NeueDNSIP3;
        NeueDNSIP3 = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeueDNSIP3 < 0 || NeueDNSIP3 > 255) {
          ERRORTEXT="DNS-Server IP-Adresse Teil 3 ist falsch!";
        }

        //17. Wert extrahieren (DNS Server IP-Adresse Teil 4)
        int NeueDNSIP4;
        NeueDNSIP4 = RequestString.substring(RequestString.lastIndexOf("=")+1).toInt();

        if(NeueDNSIP4 < 0 || NeueDNSIP4 > 255) {
          ERRORTEXT="DNS-Server IP-Adresse Teil 4 ist falsch!";
        }
        
        if(ERRORTEXT == "") {
          EEPROM.begin(512);
  
          delay(10);
  
          //Alte Konfigurationsdaten löschen
          for (int i=4; i<21; i++) {
            EEPROM.write(i, 0);
          }
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neuen IPTYP ins EEPROM:"));
    
          EEPROM.write(4, NeuerIPTYP);
  
          Serial.print(F("schreibe: "));
          Serial.println(NeuerIPTYP); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue IP-Adresse Teil 1 ins EEPROM:"));
  
          EEPROM.write(5, NeueIP1);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueIP1); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue IP-Adresse Teil 2 ins EEPROM:"));
  
          EEPROM.write(6, NeueIP2);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueIP2); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue IP-Adresse Teil 3 ins EEPROM:"));
  
          EEPROM.write(7, NeueIP3);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueIP3); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue IP-Adresse Teil 4 ins EEPROM:"));
  
          EEPROM.write(8, NeueIP4);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueIP4); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Gateway IP-Adresse Teil 1 ins EEPROM:"));
  
          EEPROM.write(9, NeueGWIP1);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueGWIP1); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Gateway IP-Adresse Teil 2 ins EEPROM:"));
  
          EEPROM.write(10, NeueGWIP2);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueGWIP2); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Gateway IP-Adresse Teil 3 ins EEPROM:"));
  
          EEPROM.write(11, NeueGWIP3);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueGWIP3); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Gateway IP-Adresse Teil 4 ins EEPROM:"));
  
          EEPROM.write(12, NeueGWIP4);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueGWIP4); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Subnet Mask Teil 1 ins EEPROM:"));
  
          EEPROM.write(13, NeueMASK1);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueMASK1); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Subnet Mask Teil 2 ins EEPROM:"));
  
          EEPROM.write(14, NeueMASK2);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueMASK2); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Subnet Mask Teil 3 ins EEPROM:"));
  
          EEPROM.write(15, NeueMASK3);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueMASK3); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue Subnet Mask Teil 4 ins EEPROM:"));
  
          EEPROM.write(16, NeueMASK4);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueMASK4); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue DNS Server IP-Adresse Teil 1 ins EEPROM:"));
  
          EEPROM.write(17, NeueDNSIP1);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueDNSIP1); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue DNS Server IP-Adresse Teil 2 ins EEPROM:"));
  
          EEPROM.write(18, NeueDNSIP2);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueDNSIP2); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue DNS Server IP-Adresse Teil 3 ins EEPROM:"));
  
          EEPROM.write(19, NeueDNSIP3);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueDNSIP3); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue DNS Server IP-Adresse Teil 4 ins EEPROM:"));
  
          EEPROM.write(20, NeueDNSIP4);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeueDNSIP4); 

          EEPROM.commit();
          EEPROM.end();
        } else {
          step=2;
        }
      }

      if (RequestString.startsWith("/settsconfig?")) {
        step=4;
        
        //1. Wert extrahieren (UPDATEINTERVAL)
        int StartPosition=RequestString.indexOf('=');
        StartPosition++;
        
        int EndePosition=RequestString.indexOf("&");

        int NeuesUPDATEINTERVAL = 0;
        NeuesUPDATEINTERVAL = RequestString.substring(StartPosition,EndePosition).toInt();

        if(NeuesUPDATEINTERVAL != 15 && NeuesUPDATEINTERVAL != 30 && NeuesUPDATEINTERVAL != 45 && NeuesUPDATEINTERVAL != 60) {
          ERRORTEXT="Falsches Update Interval!";
        }
        
        //2. Wert extrahieren (THINGSPEAKAPIKEY)
        String NeuerTHINGSPEAKAPIKEY; 

        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);
        
        NeuerTHINGSPEAKAPIKEY = RequestString.substring(StartPosition,EndePosition);

        if(NeuerTHINGSPEAKAPIKEY.length() > 16) {
          ERRORTEXT="Falscher ThingSpeak API Key!";
        }

        if(NeuerTHINGSPEAKAPIKEY == "") {
          ERRORTEXT="ThingSpeak API Key fehlt!";
        }
        
        //3. Wert extrahieren (PUSHINGBOXID)
        String NeuePUSHINGBOXID; 
        NeuePUSHINGBOXID = RequestString.substring(RequestString.lastIndexOf("=")+1);

        if(NeuePUSHINGBOXID.length() > 16) {
          ERRORTEXT="Falsche Pushingbox Scenario Device-ID!";
        }

        if(NeuePUSHINGBOXID == "") {
          ERRORTEXT="Pushingbox Scenario Device-ID fehlt!";
        }

        if(ERRORTEXT == "") {
          EEPROM.begin(512);
  
          delay(10);
          
          //Alte Konfigurationsdaten löschen
          for (int i=239; i<272; i++) {
            EEPROM.write(i, 0);
          }
    
          Serial.println(F(""));
          Serial.println(F("Schreibe neues UPDATEINTERVAL ins EEPROM:"));
  
          EEPROM.write(1, NeuesUPDATEINTERVAL);
          
          Serial.print(F("schreibe: "));
          Serial.println(NeuesUPDATEINTERVAL); 
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neuen THINGSPEAKAPIKEY ins EEPROM:"));
  
          for (int i = 0; i < NeuerTHINGSPEAKAPIKEY.length(); ++i){
            EEPROM.write(239+i, NeuerTHINGSPEAKAPIKEY[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeuerTHINGSPEAKAPIKEY[i]); 
          }    
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue PUSHINGBOXID ins EEPROM:"));
  
          for (int i = 0; i < NeuePUSHINGBOXID.length(); ++i){
            EEPROM.write(256+i, NeuePUSHINGBOXID[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeuePUSHINGBOXID[i]); 
          }    
  
          EEPROM.commit();
          EEPROM.end();
        } else {
          step=4;
        }
      }
      
      if (RequestString.startsWith("/setsensorconfig?")) {
        step=5;

        //1. Wert extrahieren (STANDORT)
        int StartPosition=RequestString.indexOf('=');
        StartPosition++;
        
        int EndePosition=RequestString.indexOf("&");

        String NeuerSTANDORT; 
        NeuerSTANDORT = RequestString.substring(StartPosition,EndePosition);

        //2. Wert extrahieren (SENSORSSID)
        StartPosition=RequestString.indexOf("=",EndePosition);
        StartPosition++;
        
        EndePosition=RequestString.indexOf("&",EndePosition+2);

        String NeueSENSORSSID = "";
        NeueSENSORSSID = RequestString.substring(StartPosition,EndePosition);

        //3. Wert extrahieren (SENSORPWD)
        String NeuesSENSORPWD = "";
        NeuesSENSORPWD = RequestString.substring(RequestString.lastIndexOf("=")+1);

        if(ERRORTEXT == "") {
          EEPROM.begin(512);
  
          delay(10);
          
          //Alte Konfigurationsdaten löschen
          for (int i=117; i<238; i++) {
            EEPROM.write(i, 0);
          }
   
          Serial.println(F(""));
          Serial.println(F("Schreibe neuen STANDORT ins EEPROM:"));
    
          for (int i = 0; i < NeuerSTANDORT.length(); ++i){
            EEPROM.write(213+i, NeuerSTANDORT[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeuerSTANDORT[i]); 
          }    
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neue SENSORSSID ins EEPROM:"));
  
          for (int i = 0; i < NeueSENSORSSID.length(); ++i){
            EEPROM.write(117+i, NeueSENSORSSID[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeueSENSORSSID[i]); 
          }    
  
          Serial.println(F(""));
          Serial.println(F("Schreibe neues SENSORPWD ins EEPROM:"));
  
          for (int i = 0; i < NeuesSENSORPWD.length(); ++i){
            EEPROM.write(149+i, NeuesSENSORPWD[i]);
            
            Serial.print(F("schreibe: "));
            Serial.println(NeuesSENSORPWD[i]); 
          }    
                   
          Serial.println(F("Schreibe neues FIRSTSTARTFLAG ins EEPROM:"));
          FIRSTSTARTFLAG=111;
          
          EEPROM.write(0, FIRSTSTARTFLAG);
  
          Serial.print(F("schreibe: "));
          Serial.println(FIRSTSTARTFLAG); 
  
          EEPROM.commit();
          EEPROM.end();

          //RTC-Ram Flags neu setzen und speichern
          RAMFlags[0]=1;    //Grundwert für Startmodus auf Normalstart setzen
          RAMFlags[1]=99;   //Grundwert für Wakeups auf 99 setzen damit nach Neustart direkt die erste Übertragung folgt
          
          while(system_rtc_mem_write(RTC_BASE,RAMFlags,RTC_RAM_ARRAY_SIZE)!=true) {
            delay(1);
          }
        } else {
          step=4;
        }
      }

      if (step == 5) {
        Serial.println(F("Abschlussseite des Setup wird ausgegeben"));
        Serial.println(F(""));
        
        HTMLSite ="<!DOCTYPE html><html><head>";
        HTMLSite += "<title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "<br /><br /><fieldset style='width:450px'><br />";
        HTMLSite += "<b>Das Setup des Sensors<br />";
        HTMLSite += "ist abgeschlossen.<br /><br />";
        HTMLSite += "Der Sensor startet jetzt neu!<br />";
        HTMLSite += "<br /></fieldset></center></body></html>\r\n\r\n";

        HTMLHeader  = "HTTP/1.1 200 OK\r\n";
        HTMLHeader += "Content-Length: ";
        HTMLHeader += HTMLSite.length();
        HTMLHeader += "\r\n";
        HTMLHeader += "Content-Type: text/html\r\n";
        HTMLHeader += "Connection: close\r\n";
        HTMLHeader += "\r\n";
        
        client.print(HTMLHeader);
        delay(100);
        client.print(HTMLSite);
        delay(100);
        
        client.stop();

        WiFi.disconnect();

        ESP.deepSleep(1000,WAKE_RF_DEFAULT); //System neu starten über Deep-Sleep Timer
        delay(100);
      }
      
      if (step == 1) {
        Serial.println(F("WLAN Konfigurationsseite wird ausgegeben"));
        Serial.println(F(""));

        HTMLSite ="<!DOCTYPE html><html><head><title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        
        if(ERRORTEXT != "") {
          HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Es ist ein Fehler aufgetreten!</legend>";
          HTMLSite += "<font color='red'>";
          HTMLSite += ERRORTEXT;
          HTMLSite += "</font><br /></fieldset>";
        }

        HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Sensor Setup Schritt 1</legend>";
        HTMLSite += "<form method='GET' action='setwlanconfig'>";
        HTMLSite += "<br /><b>WLAN Einstellungen</b><br /><hr /><br />";
        HTMLSite += HTMLFORMULARWLAN;
        HTMLSite += "<br />WLAN-Passwort: <input name='WLANPWD' type='text' size='30' maxlength='64' value='";
        HTMLSite += WLANPWD.c_str();
        HTMLSite += "'></input><br />";
        HTMLSite += "<br />Verbindungs-Timeout: ";
        HTMLSite += "<select name='CONNECTIONTIMEOUT' size='1'>";

        if (CONNECTIONTIMEOUT==10) {
          HTMLSite += "<option value='10' selected>10</option><option value='30'>30</option><option value='60'>60</option>";
        } else if (CONNECTIONTIMEOUT==30) {
          HTMLSite += "<option value='10'>10</option><option value='30' selected>30</option><option value='60'>60</option>";
        } else {
          HTMLSite += "<option value='10'>10</option><option value='30'>30</option><option value='60' selected>60</option>";
        }
 
        HTMLSite += "</select> Sekunden<br />";
        HTMLSite += "<br /></fieldset>";
        HTMLSite += "<br /><input type='submit' value='Einstellungen speichern und weiter zu Schritt 2...'></form>";
        HTMLSite += "</center></body></html>\r\n\r\n";
      }
      
      if (step == 2) {
        Serial.println(F("IP Konfigurationsseite wird ausgegeben"));
        Serial.println(F(""));

        HTMLSite ="<!DOCTYPE html><html><head><title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        
        if(ERRORTEXT != "") {
          HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Es ist ein Fehler aufgetreten!</legend>";
          HTMLSite += "<font color='red'>";
          HTMLSite += ERRORTEXT;
          HTMLSite += "</font><br /></fieldset>";
        }

        HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Sensor Setup Schritt 2</legend>";
        HTMLSite += "<form method='GET' action='setipconfig'>";
        HTMLSite += "<br /><b>IP Einstellungen</b><br /><hr />";
        HTMLSite += "<br />IP-Adresstyp: ";
        HTMLSite += "<select name='STATICIP' size='1'>";
        
        if (STATICIP==0) {
          HTMLSite += "<option value='0' selected>DHCP</option><option value='1'>Statisch</option></select>";
        } else {
          HTMLSite += "<option value='0'>DHCP</option><option value='1' selected>Statisch</option></select>";
        }
        HTMLSite += "<br /><br />IP-Adresse: <input name='IP1' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(ip[0]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='IP2' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(ip[1]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='IP3' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(ip[2]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='IP4' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(ip[3]);
        HTMLSite += "'></input>";
        HTMLSite += "<br /><br />Gateway IP-Adresse: <input name='GWIP1' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(gw[0]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='GWIP2' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(gw[1]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='GWIP3' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(gw[2]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='GWIP4' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(gw[3]);
        HTMLSite += "'></input>";
        HTMLSite += "<br /><br />Subnet Mask: <input name='MASK1' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(sub[0]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='MASK2' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(sub[1]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='MASK3' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(sub[2]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='MASK4' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(sub[3]);
        HTMLSite += "'></input>";
        HTMLSite += "<br /><br />DNS-Server IP-Adresse: <input name='DNSIP1' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(dns[0]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='DNSIP2' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(dns[1]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='DNSIP3' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(dns[2]);
        HTMLSite += "'></input>.";
        HTMLSite += "<input name='DNSIP4' type='text' size='3' maxlength='3' value='";
        HTMLSite += String(dns[3]);
        HTMLSite += "'></input><br />";
        HTMLSite += "<br /></fieldset>";
        HTMLSite += "<br /><input type='submit' value='Einstellungen speichern und weiter zu Schritt 3...'></form>";
        HTMLSite += "</center></body></html>\r\n\r\n";
      } 
      
      if (step == 3) {
        Serial.println(F("Thingspeak Konfigurationsseite wird ausgegeben"));
        Serial.println(F(""));

        HTMLSite ="<!DOCTYPE html><html><head><title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        
        if(ERRORTEXT != "") {
          HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Es ist ein Fehler aufgetreten!</legend>";
          HTMLSite += "<font color='red'>";
          HTMLSite += ERRORTEXT;
          HTMLSite += "</font><br /></fieldset>";
        }

        HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Sensor Setup Schritt 3</legend>";
        HTMLSite += "<form method='GET' action='settsconfig'>";
        HTMLSite += "<br /><b>Thingspeak Einstellungen</b><br /><hr />";
        HTMLSite += "<br />&Uuml;bertrag alle ";
        HTMLSite += "<select name='UPDATEINTERVAL' size='1'>";

        if (UPDATEINTERVAL==15) {
          HTMLSite += "<option value='15' selected>15</option><option value='30'>30</option><option value='45'>45</option><option value='60'>60</option>";
        } else if (UPDATEINTERVAL==30) {  
          HTMLSite += "<option value='15'>15</option><option value='30' selected>30</option><option value='45'>45</option><option value='60'>60</option>";
        } else if (UPDATEINTERVAL==45) {  
          HTMLSite += "<option value='15'>15</option><option value='30'>30</option><option value='45' selected>45</option><option value='60'>60</option>";
        } else {  
          HTMLSite += "<option value='15'>15</option><option value='30'>30</option><option value='45'>45</option><option value='60' selected>60</option>";
        }

        HTMLSite += "</select> Minuten";
        HTMLSite += "<br /><br />API Write-Key: <input name='THINGSPEAKAPIKEY' type='text' size='20' maxlength='16' value='";
        HTMLSite += THINGSPEAKAPIKEY.c_str();
        HTMLSite += "'></input><br /><hr />";
        HTMLSite += "<br /><b>Pushingbox Einstellungen</b><br /><hr />";
        HTMLSite += "<br />Scenario Device-ID: <input name='PUSHINGBOXID' type='text' size='20' maxlength='16' value='";
        HTMLSite += PUSHINGBOXID.c_str();
        HTMLSite += "'></input><br />";
        HTMLSite += "<br /></fieldset>";
        HTMLSite += "<br /><input type='submit' value='Einstellungen speichern und weiter zu Schritt 4...'></form>";
        HTMLSite += "</center></body></html>\r\n\r\n";
      } 
      
      if (step == 4) {
        Serial.println(F("Sensor Konfigurationsseite wird ausgegeben"));
        Serial.println(F(""));

        HTMLSite ="<!DOCTYPE html><html><head><title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        
        if(ERRORTEXT != "") {
          HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Es ist ein Fehler aufgetreten!</legend>";
          HTMLSite += "<font color='red'>";
          HTMLSite += ERRORTEXT;
          HTMLSite += "</font><br /></fieldset>";
        }

        HTMLSite += "<br /><br /><fieldset style='width:450px'><legend>Sensor Setup Schritt 4</legend>";
        HTMLSite += "<form method='GET' action='setsensorconfig'>";
        HTMLSite += "<br /><b>Sensor Einstellungen</b><br /><hr />";
        HTMLSite += "<br />Standort: <input name='SENSORSTANDORT' type='text' size='25' maxlength='25' value='";
        HTMLSite += SENSORSTANDORT.c_str();
        HTMLSite += "'></input><br />";
        HTMLSite += "<br />SSID: <input name='SENSORSSID' type='text' size='32' maxlength='32' value='";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "'></input><br />";
        HTMLSite += "<br />Passwort: <input name='SENSORPWD' type='text' size='30' maxlength='64' value='";
        HTMLSite += SENSORPWD.c_str();
        HTMLSite += "'></input><br /><br /></fieldset>";
        HTMLSite += "<br /><input type='submit' value='Einstellungen speichern und das Setup beenden'></form>";
        HTMLSite += "</center></body></html>\r\n\r\n";
      } 

      if (HTMLSite == "" && step == 0) {
        step=1;
        
        Serial.println(F("Setup Startseite wird ausgegeben"));
        Serial.println(F(""));

        HTMLSite ="<!DOCTYPE html><html><head><title>";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "</title>";
        HTMLSite += "</head><body style='font-family:Verdana, Arial, Sans-Serif'><center>";
        HTMLSite += "<img src='logo.png' border='0' /><br /><br />";
        HTMLSite += SENSORSSID.c_str();
        HTMLSite += "<br /><br /><fieldset style='width:450px'><br />";
        HTMLSite += "<b>Herzlich Willkommen zum Setup<br />";
        HTMLSite += "des ESP8266 DHT-22 Akku Sensors.<br /><br />";
        HTMLSite += "Sie werden jetzt Schritt f&uuml;r Schritt<br />";
        HTMLSite += "durch das Setup gef&uuml;hrt.<br /><br /></fieldset><br />";
        HTMLSite += "<form method='GET' action='wlanconfig.htm'>";
        HTMLSite += "<input type='submit' value='Weiter zu Schritt 1...'></form>";
        HTMLSite += "</center></body></html>\r\n\r\n";
      }

      if (HTMLSite.length() > 0) {
        HTMLHeader  = "HTTP/1.1 200 OK\r\n";
        HTMLHeader += "Content-Length: ";
        HTMLHeader += HTMLSite.length();
        HTMLHeader += "\r\n";
        HTMLHeader += "Content-Type: text/html\r\n";
        HTMLHeader += "Connection: close\r\n";
        HTMLHeader += "\r\n";
        
        client.print(HTMLHeader);
        delay(10);
        client.print(HTMLSite);
        delay(10);
      }
    }
  }

  delay(100);
}

