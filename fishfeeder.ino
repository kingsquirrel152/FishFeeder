/*
 *  ESP based fishfeeder firmware
 *  Copyright @ 2016 Alex Stewart
 *  
 *  As long as you retain this notice you can do whatever you want with this stuff. 
 *  
 */
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUDP.h>
#include <Servo.h>
#include <EEPROM.h>


const char* ssid            = Insert SSID here;
const char* password        = Insert passwd here;
const char* host            = "hooks.slack.com";
const char* SlackWebhookURL = /services/YourWebHookHERE;

WiFiUDP ntpUDP;

// By default 'time.nist.gov' is used with 60 seconds update interval and
NTPClient timeClient(ntpUDP, -4*60*60); // - 4 hours for EST

// You can specify the time server pool and the offset, (in seconds)
// additionaly you can specify the update interval (in milliseconds).
// NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
// + 4 hours

// SSL Certificate finngerprint for the host, this will be invalid you may update it if you like
const char* fingerprint = "‎‎ab f0 5b a9 1a e0 ae 5f ce 32 2e 7c 66 67 49 ec dd 6d 6a 38";

uint8_t pos = 0;      //Current feeder position 
uint8_t prom_pos = 0; //Stored last postion in EEPROM


Servo fishServo; //Servo object for running the feeder
char bins[16];   //Finite servo position 

#define START 5  //Start at BIN5

void setup(){
  
  Serial.begin(115200);

  //Initialize WIFI
  WiFi.begin(ssid, password);

  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );
  }

  //Initialize NTP 
  timeClient.begin();
  timeClient.update();


  Serial.println("");
  Serial.println("WiFi connected");  
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  //Tell Slack that we booted, and the current NTP time
  sendToSlack("STARTing up current NTP time " + timeClient.getFormattedTime());

  //Initialize things from EEPROM (in the event of a power failure we want to make sure 
  //we dont accidently over or under feed when power returns)
  EEPROM.begin(512);
  delay(100);
  prom_pos = (uint8_t) EEPROM.read(1);
  printf("Stored pos %d\n", prom_pos);

  char buf[8];
  itoa(prom_pos, buf, 10);

  //Feed the stored EEPROM position to slack
  sendToSlack("Stored bin pos " + String(buf));

  //Push button input 
  pinMode(14, INPUT_PULLUP);
  printf("pin %d\n", digitalRead(14));
 
  //Initialize bin positions I manually calcuated this using a servo test firmware drop
  bins[0] = 17;
  bins[1] = 27;
  bins[2] = 37;
  bins[3] = bins[2] + 10;
  bins[4] = bins[3] + 11;
  bins[5] = bins[4] + 12;
  bins[6] = bins[5] + 10;
  bins[7] = bins[6] + 15;
  bins[8] = bins[7] + 13;
  bins[9] = bins[8] + 12;
  bins[10] = bins[9] + 15;
  bins[11] = bins[10] + 10;
  bins[12] = bins[11] + 11;
  bins[13] = bins[12] + 13;
  bins[14] = bins[13] + 14;
  bins[15] = bins[14] + 10;

  //Connect up the servo
  fishServo.attach(16);

  //If stored position isnt 0, then odds are we need to transition (slowly) from where eeprom thinks we are
  //back to start (which means going through "empty" position (no food)
  //Once we're homed - we can from from START to where we should be.
  if( prom_pos != 0)
  {
    transition(bins[prom_pos-1], START, 300);
    transition(START, bins[prom_pos-1], 300);
  }
  else 
  {
    transition(bins[prom_pos-1], START, 300);
  }
  pos = prom_pos; //Once "homed" mark that we have a valid position 
}

int i = 0;

bool fired = false;
void loop() {
  
  //Every 30 seconds make sure NTP time is up to date
  if( i == 30) {        
    timeClient.update();
    i = 0;
  }
  else {
    i++;
  }

  //Print what time we think it is (great for debu)
  Serial.println(timeClient.getFormattedTime());

  /***Trigger******/
  delay(1000);
  int hours = timeClient.getHours();
  int minutes = timeClient.getMinutes();
  int sec = timeClient.getSeconds();

  //Below is a twice a day trigger (8AM/8PM) feedings
  if( (hours == 8) || (hours == 20)) {
    if(minutes == 0){ //During the 0 minute 
      if( (sec >= 0) && (sec <= 10)) {  // 10 second window to "fire" the feed cycle
          if( !fired) {
            Serial.print("connecting to ");
            Serial.println(host);
            sendToSlack("Feeding the fish!");
            trigger();
            fired = true;
          }
       }
       else 
       {
        Serial.print("Reset Trigger");
        fired = false;
       }
    }
  }
  /***********/
  //Reset monitor - if button is pressed - we reset the wheel back to position 0
  if( digitalRead(14) == 0) 
  {
    transition(bins[pos], START, 300);
    pos = 0;
    printf("Reset Saving %d\n", pos );
    EEPROM.begin(512);
    delay(100);
    EEPROM.write(1, pos);
    EEPROM.commit();
    EEPROM.end();
    delay(2000);
  }
}

//Handle Step logic for wheel and trip the "knock" sequence (drive piezo beeper)
void trigger()
{
    knock(); //Let our friends know its feeding time

    //Move the feeder
    if( pos == 0 )
    {
       transition(START, bins[pos], 300);
    }
    else
    {
      transition(bins[pos-1], bins[pos], 300);
    }

    //Bump next position 
    if( pos < 15)
    {
      pos++;
    }
    else //If we're past the end, cycle back to the beginning 
    {
      transition(bins[pos], START, 300);
      pos = 0;
    }

    //Save State -  best effort power failure mitigation 
    printf("Saving %d\n", pos );
    EEPROM.begin(512);
    delay(100);
    EEPROM.write(1, pos);
    EEPROM.commit();
    EEPROM.end();
    delay(2000);                       // waits 15ms for the servo to reach the position
}

//Helper to smoothly drive servo from start postion to finish position. 
//Linear steps from start to finish, while sleeping for pause mS
void transition(char start, char finish, char pause)
{
  int pos;
  if( finish > start)
  {
    for(pos = start; pos <= finish; pos++)
    {
      fishServo.write(pos);
      printf("pos %d\n", pos);
      delay(pause);
    }
  }
  else
  {
    for(pos = start; pos >= finish; pos--)
    {
      fishServo.write(pos);
      printf("pos %d\n", pos);
      delay(pause);
    }
  }
}


//Plays a sequence of thee tones (lets our friends know food is coming)
void knock()
{
    tone(13, 1000);
    delay(200);
    noTone(13);
    delay(300);

    tone(13, 1000);
    delay(200);
    noTone(13);
    delay(300);

    tone(13, 1000);
    delay(200);
    noTone(13);
    delay(300);
}

//Sends the message provided to slack 
void sendToSlack(String message)
{
    String channel = "#dafish";
    String username = "Balooney";

    // create a secure connection using WiFiClientSecure
   WiFiClientSecure client;
   const int httpPort = 443;
   if (client.connect(host, httpPort)) {

     // verify the signature of the ssl certificate
     if (client.verify(fingerprint, host)) {
      Serial.println("ssl cert matches");
     } else {
      Serial.println("ssl cert mismatch");
     }
 
     String PostData="payload={\"channel\": \"" + channel + "\", \"username\": \"" + username + "\", \"text\": \"" + message + "\", \"icon_emoji\": \":ghost:\"}";
     Serial.println(PostData);
     
     client.print("POST ");
     client.print(SlackWebhookURL);
     client.println(" HTTP/1.1");
     client.print("Host: ");
     client.println(host);
     client.println("User-Agent: ArduinoIoT/1.0");
     client.println("Connection: close");
     client.println("Content-Type: application/x-www-form-urlencoded;");
     client.print("Content-Length: ");
     client.println(PostData.length());
     client.println();
     client.println(PostData);
       
      delay(500);
 
     // Read all the lines of the reply from server and print them to Serial for debugging
     while(client.available()){
     String line = client.readStringUntil('\r');
     Serial.print(line);
     }
 
     Serial.println();
     Serial.println("closing connection"); 
    }
}

