/* OpenGarage Firmware
 *
 * Main loop
 * Mar 2016 @ OpenGarage.io
 *
 * This file is part of the OpenGarage library
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if defined(SERIAL_DEBUG)
  #define BLYNK_DEBUG
  #define BLYNK_PRINT Serial
#endif

#include "OpenGarage.h"
#include "espconnect.h"
#include <BlynkSimpleEsp8266.h>
#include <PubSubClient.h> //https://github.com/Imroy/pubsubclient

OpenGarage og;
ESP8266WebServer *server = NULL;

WidgetLED blynk_led(BLYNK_PIN_LED);
WidgetLCD blynk_lcd(BLYNK_PIN_LCD);

static WiFiClient wificlient;
PubSubClient mqttclient(wificlient);


static String scanned_ssids;
static byte read_cnt = 0;
static uint distance = 0;
static byte door_status = 0;
static bool curr_cloud_access_en = false;
static bool curr_local_access_en = false;
static uint led_blink_ms = LED_FAST_BLINK;
static ulong restart_timeout = 0;
static ulong justopen_timestamp = 0;
static byte curr_mode;
// this is one byte storing the door status histogram
// maximum 8 bits
static byte door_status_hist = 0;
static ulong curr_utc_time = 0;
static ulong curr_utc_hour= 0;
static HTTPClient http;

void do_setup();

void server_send_html(String html) {
  server->send(200, "text/html", html);
}

void server_send_result(byte code, const char* item = NULL) {
  String html = F("{\"result\":");
  html += code;
  if (!item) item = "";
  html += F(",\"item\":\"");
  html += item;
  html += F("\"");
  html += F("}");
  server_send_html(html);
}

bool get_value_by_key(const char* key, uint& val) {
  if(server->hasArg(key)) {
    val = server->arg(key).toInt();   
    return true;
  } else {
    return false;
  }
}

bool get_value_by_key(const char* key, String& val) {
  if(server->hasArg(key)) {
    val = server->arg(key);   
    return true;
  } else {
    return false;
  }
}

void on_home()
{
  String html = "";
  
  if(curr_mode == OG_MOD_AP) {
    html += FPSTR(html_mobile_header); 
    html += FPSTR(html_ap_home);
  } else {
    html += FPSTR(html_jquery_header);
    html += FPSTR(html_sta_home);
  }
  server_send_html(html);
}

void on_sta_view_options() {
  if(curr_mode == OG_MOD_AP) return;
  String html = FPSTR(html_jquery_header);
  html += FPSTR(html_sta_options);
  server_send_html(html);
}

void on_sta_view_logs() {
  if(curr_mode == OG_MOD_AP) return;
  String html = FPSTR(html_jquery_header);
  html += FPSTR(html_sta_logs);
  server_send_html(html);
}

char dec2hexchar(byte dec) {
  if(dec<10) return '0'+dec;
  else return 'A'+(dec-10);
}

String get_mac() {
  static String hex = "";
  if(!hex.length()) {
    byte mac[6];
    WiFi.macAddress(mac);

    for(byte i=0;i<6;i++) {
      hex += dec2hexchar((mac[i]>>4)&0x0F);
      hex += dec2hexchar(mac[i]&0x0F);
      if(i!=5) hex += ":";
    }
  }
  return hex;
}

String get_ap_ssid() {
  static String ap_ssid = "";
  if(!ap_ssid.length()) {
    byte mac[6];
    WiFi.macAddress(mac);
    ap_ssid = "OG_";
    for(byte i=3;i<6;i++) {
      ap_ssid += dec2hexchar((mac[i]>>4)&0x0F);
      ap_ssid += dec2hexchar(mac[i]&0x0F);
    }
  }
  return ap_ssid;
}

String get_ip() {
  String ip = "";
  IPAddress _ip = WiFi.localIP();
  ip = _ip[0];
  ip += ".";
  ip += _ip[1];
  ip += ".";
  ip += _ip[2];
  ip += ".";
  ip += _ip[3];
  return ip;
}

void on_sta_controller() {
  if(curr_mode == OG_MOD_AP) return;
  String html = "";
  html += F("{\"dist\":");
  html += distance;
  html += F(",\"door\":");
  html += door_status;
  html += F(",\"rcnt\":");
  html += read_cnt;
  html += F(",\"fwv\":");
  html += og.options[OPTION_FWV].ival;
  html += F(",\"name\":\"");
  html += og.options[OPTION_NAME].sval;
  html += F("\",\"mac\":\"");
  html += get_mac();
  html += F("\",\"cid\":");
  html += ESP.getChipId();
  html += F(",\"rssi\":");
  html += (int16_t)WiFi.RSSI();
  html += F("}");
  server_send_html(html);
}

void on_sta_logs() {
  if(curr_mode == OG_MOD_AP) return;
  String html = "";
  html += F("{\"name\":\"");
  html += og.options[OPTION_NAME].sval;
  html += F("\",\"time\":");
  html += curr_utc_time;
  html += F(",\"logs\":[");
  if(!og.read_log_start()) {
    html += F("]}");
    server_send_html(html);
    return;
  }
  LogStruct l;
  for(uint i=0;i<MAX_LOG_RECORDS;i++) {
    if(!og.read_log_next(l)) break;
    if(!l.tstamp) continue;
    html += F("[");
    html += l.tstamp;
    html += F(",");
    html += l.status;
    html += F(",");
    html += l.dist;
    html += F("],");
  }
  og.read_log_end();
  html.remove(html.length()-1); // remove the extra ,
  html += F("]}");
  server_send_html(html);
}

bool verify_device_key() {
  if(server->hasArg("dkey") && (server->arg("dkey") == og.options[OPTION_DKEY].sval))
    return true;
  return false;
}

void on_sta_change_controller() {
  if(curr_mode == OG_MOD_AP) return;

  if(!verify_device_key()) {
    server_send_result(HTML_UNAUTHORIZED);
    return;
  }
  if(server->hasArg("click")) {
    server_send_result(HTML_SUCCESS);
    if(!og.options[OPTION_ALM].ival) {
      // if alarm is not enabled, trigger relay right away
      og.click_relay();
    } else {
      // else, set alarm
      og.set_alarm();
    }
  } else if(server->hasArg("reboot")) {
    server_send_result(HTML_SUCCESS);
    restart_timeout = millis() + 1000;
    og.state = OG_STATE_RESTART;
  } else if(server->hasArg("apmode")) {
    server_send_result(HTML_SUCCESS);
    og.reset_to_ap();
  } else {
    server_send_result(HTML_NOT_PERMITTED);
  }
}

void on_sta_change_options() {
  if(curr_mode == OG_MOD_AP) return;
  
  if(!verify_device_key()) {
    server_send_result(HTML_UNAUTHORIZED);
    return;
  }
  uint ival = 0;
  String sval;
  byte i;
  OptionStruct *o = og.options;
  
  byte usi = 0;
  // FIRST ROUND: check option validity
  // do not save option values yet
  for(i=0;i<NUM_OPTIONS;i++,o++) {
    const char *key = o->name.c_str();
    // these options cannot be modified here
    if(i==OPTION_FWV || i==OPTION_MOD  || i==OPTION_SSID ||
      i==OPTION_PASS || i==OPTION_DKEY)
      continue;
    
    if(o->max) {  // integer options
      if(get_value_by_key(key, ival)) {
        if(ival>o->max) {
          server_send_result(HTML_DATA_OUTOFBOUND, key);
          return;
        }
        if(i==OPTION_CDT && ival < 50) {
          // click delay time should be at least 50 ms
          server_send_result(HTML_DATA_OUTOFBOUND, key);
          return;
        }
        if(i==OPTION_USI && ival==1) {
          // mark device IP and gateway IP change
          usi = 1;
        }
      }
    }
  }
  
  // Check device IP and gateway IP changes
  String dvip, gwip, subn;
  const char* _dvip = "dvip";
  const char* _gwip = "gwip";
  const char* _subn = "subn";
  if(usi) {
    if(get_value_by_key(_dvip, dvip)) {
      if(get_value_by_key(_gwip, gwip)) {
        // check validity of IP address
        IPAddress ip;
        if(!ip.fromString(dvip)) {server_send_result(HTML_DATA_FORMATERROR, _dvip); return;}
        if(!ip.fromString(gwip)) {server_send_result(HTML_DATA_FORMATERROR, _gwip); return;}
        if(get_value_by_key(_subn, subn)) {
          if(!ip.fromString(subn)) {
            server_send_result(HTML_DATA_FORMATERROR, _subn);
            return;
          }
        }
      } else {
        server_send_result(HTML_DATA_MISSING, _gwip);
        return;
      }              
    } else {
      server_send_result(HTML_DATA_MISSING, _dvip);
      return;
    }
  }
  // Check device key change
  String nkey, ckey;
  const char* _nkey = "nkey";
  const char* _ckey = "ckey";
  
  if(get_value_by_key(_nkey, nkey)) {
    if(get_value_by_key(_ckey, ckey)) {
      if(!nkey.equals(ckey)) {
        server_send_result(HTML_MISMATCH, _ckey);
        return;
      }
    } else {
      server_send_result(HTML_DATA_MISSING, _ckey);
      return;
    }
  }
  
  // SECOND ROUND: change option values
  o = og.options;
  for(i=0;i<NUM_OPTIONS;i++,o++) {
    const char *key = o->name.c_str();
    // these options cannot be modified here
    if(i==OPTION_FWV || i==OPTION_MOD  || i==OPTION_SSID ||
      i==OPTION_PASS || i==OPTION_DKEY)
      continue;
    
    if(o->max) {  // integer options
      if(get_value_by_key(key, ival)) {
        o->ival = ival;
      }
    } else {
      if(get_value_by_key(key, sval)) {
        o->sval = sval;
      }
    }
  }

  if(usi) {
    get_value_by_key(_dvip, dvip);
    get_value_by_key(_gwip, gwip);
    og.options[OPTION_DVIP].sval = dvip;
    og.options[OPTION_GWIP].sval = gwip;
    if(get_value_by_key(_subn, subn)) {
      og.options[OPTION_SUBN].sval = subn;
    }
  }
  
  if(get_value_by_key(_nkey, nkey)) {
      og.options[OPTION_DKEY].sval = nkey;
  }

  og.options_save();
  server_send_result(HTML_SUCCESS);
}

void on_sta_options() {
  if(curr_mode == OG_MOD_AP) return;
  String html = "{";
  OptionStruct *o = og.options;
  for(byte i=0;i<NUM_OPTIONS;i++,o++) {
    if(!o->max) {
      if(i==OPTION_PASS || i==OPTION_DKEY) { // do not output password or device key
        continue;
      } else {
        html += F("\"");
        html += o->name;
        html += F("\":");
        html += F("\"");
        html += o->sval;
        html += F("\"");
        html += ",";
      }
    } else {  // if this is a int option
      html += F("\"");
      html += o->name;
      html += F("\":");
      html += o->ival;
      html += ",";
    }
  }
  html.remove(html.length()-1); // remove the extra ,
  html += F("}");
  server_send_html(html);
}

void on_ap_scan() {
  if(curr_mode == OG_MOD_STA) return;
  server_send_html(scanned_ssids);
}

void on_ap_change_config() {
  if(curr_mode == OG_MOD_STA) return;
  String html = FPSTR(html_mobile_header);
  if(server->hasArg("ssid")&&server->arg("ssid").length()!=0) {
    og.options[OPTION_SSID].sval = server->arg("ssid");
    og.options[OPTION_PASS].sval = server->arg("pass");
    // if cloud token is provided, save it
    if(server->hasArg("auth")&&server->arg("auth").length()!=0)
      og.options[OPTION_AUTH].sval = server->arg("auth");
    og.options_save();
    server_send_result(HTML_SUCCESS);
    og.state = OG_STATE_TRY_CONNECT;
  } else {
    server_send_result(HTML_DATA_MISSING, "ssid");
  }
}

void on_ap_try_connect() {
  if(curr_mode == OG_MOD_STA) return;
  String html = "{";
  html += F("\"ip\":");
  html += (WiFi.status() == WL_CONNECTED) ? (uint32_t)WiFi.localIP() : 0;
  html += F("}");
  server_send_html(html);
  if(WiFi.status() == WL_CONNECTED && WiFi.localIP()) {
    og.options[OPTION_MOD].ival = OG_MOD_STA;
    if(og.options[OPTION_AUTH].sval.length() == 32) {
      og.options[OPTION_ACC].ival = OG_ACC_BOTH;
    }
    og.options_save();  
    restart_timeout = millis() + 2000;
    og.state = OG_STATE_RESTART;
  }
}

// MQTT callback to read "Button" requests
void mqtt_callback(const MQTT::Publish& pub) { 
  //DEBUG_PRINT("MQTT Message Received: ");
  //DEBUG_PRINT(pub.topic());
  //DEBUG_PRINT(" Data: ");
  //DEBUG_PRINTLN(pub.payload_string());
  if (pub.payload_string() == "Button") { 								//MQTT: If "Button" in topic turn the output on/open the door 
    if(!og.options[OPTION_ALM].ival) {
      // if alarm is not enabled, trigger relay right away
      og.click_relay();
      } 
	else {
      // else, set alarm
      og.set_alarm();
      }
  } 
}

void do_setup()
{
  DEBUG_BEGIN(115200);
  if(server) {
    delete server;
    server = NULL;
  }
  WiFi.persistent(false); // turn off persistent, fixing flash crashing issue
  og.begin();
  og.options_setup();
  curr_cloud_access_en = og.get_cloud_access_en();
  curr_local_access_en = og.get_local_access_en();
  curr_mode = og.get_mode();
  if(!server) {
    server = new ESP8266WebServer(og.options[OPTION_HTP].ival);
    DEBUG_PRINT(F("server started @ "));
    DEBUG_PRINTLN(og.options[OPTION_HTP].ival);
  }
  led_blink_ms = LED_FAST_BLINK;
  
}

void process_ui()
{
  // process button
  static ulong button_down_time = 0;
  if(og.get_button() == LOW) {
    if(!button_down_time) {
      button_down_time = millis();
    } else {
      ulong curr = millis();
      if(curr > button_down_time + BUTTON_FACRESET_TIMEOUT) {
        led_blink_ms = 0;
        og.set_led(LOW);
      } else if(curr > button_down_time + BUTTON_APRESET_TIMEOUT) {
        led_blink_ms = 0;
        og.set_led(HIGH);
      }
    }
  }
  else {
    if (button_down_time > 0) {
      ulong curr = millis();
      if(curr > button_down_time + BUTTON_FACRESET_TIMEOUT) {
        og.state = OG_STATE_RESET;
      } else if(curr > button_down_time + BUTTON_APRESET_TIMEOUT) {
        og.reset_to_ap();
      } else if(curr > button_down_time + 50) {
        og.click_relay();
      }
      button_down_time = 0;
    }
  }
  // process led
  static ulong led_toggle_timeout = 0;
  if(led_blink_ms) {
    if(millis() > led_toggle_timeout) {
      // toggle led
      og.set_led(1-og.get_led());
      led_toggle_timeout = millis() + led_blink_ms;
    }
  }  
}

byte check_door_status_hist() {
  // perform pattern matching of door status histogram
  // and return the corresponding results
  const byte allones = (1<<DOOR_STATUS_HIST_K)-1;       // 0b1111
  const byte lowones = (1<<(DOOR_STATUS_HIST_K/2))-1; // 0b0011
  const byte highones= lowones << (DOOR_STATUS_HIST_K/2); // 0b1100
  
  byte _hist = door_status_hist & allones;  // get the lowest K bits
  if(_hist == 0) return DOOR_STATUS_REMAIN_CLOSED;
  if(_hist == allones) return DOOR_STATUS_REMAIN_OPEN;
  if(_hist == lowones) return DOOR_STATUS_JUST_OPENED;
  if(_hist == highones) return DOOR_STATUS_JUST_CLOSED;

  return DOOR_STATUS_MIXED;
}

void on_sta_update() {
  String html = FPSTR(html_jquery_header); 
  html += FPSTR(html_sta_update);
  server_send_html(html);
}

void on_sta_upload_fin() {

  if(!verify_device_key()) {
    server_send_result(HTML_UNAUTHORIZED);
    Update.reset();
    return;
  }

  // finish update and check error
  if(!Update.end(true) || Update.hasError()) {
    server_send_result(HTML_UPLOAD_FAILED);
    return;
  }
  
  server_send_result(HTML_SUCCESS);
  restart_timeout = millis() + 2000;
  og.state = OG_STATE_RESTART;
}

void on_sta_upload() {
  HTTPUpload& upload = server->upload();
  if(upload.status == UPLOAD_FILE_START){
    WiFiUDP::stopAll();
    Blynk.disconnect(); // disconnect Blynk during firmware upload
    DEBUG_PRINT(F("prepare to upload: "));
    DEBUG_PRINTLN(upload.filename);
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace()-0x1000)&0xFFFFF000;
    if(!Update.begin(maxSketchSpace)) {
      DEBUG_PRINTLN(F("not enough space"));
    }
  } else if(upload.status == UPLOAD_FILE_WRITE) {
    DEBUG_PRINT(".");
    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      DEBUG_PRINTLN(F("size mismatch"));
    }
      
  } else if(upload.status == UPLOAD_FILE_END) {
    
    DEBUG_PRINTLN(F("upload completed"));
       
  } else if(upload.status == UPLOAD_FILE_ABORTED){
    Update.end();
    DEBUG_PRINTLN(F("upload aborted"));
  }
  delay(0);    
}

void check_status_ap() {
  static ulong cs_timeout = 0;
  if(millis() > cs_timeout) {
    DEBUG_PRINTLN(og.read_distance());
    DEBUG_PRINTLN(OG_FWV);
    cs_timeout = millis() + 2000;
  }
}

bool mqtt_connect_subscibe() {
  static ulong mqtt_subscribe_timeout = 0;
  if(curr_utc_time > mqtt_subscribe_timeout) {
    if (!mqttclient.connected()) {
      DEBUG_PRINTLN("MQTT Not connected- (Re)connect MQTT");
      mqttclient.set_server(og.options[OPTION_MQTT].sval, 1883);
      if (mqttclient.connect(og.options[OPTION_NAME].sval)) {
        mqttclient.set_callback(mqtt_callback); 		
        mqttclient.subscribe(og.options[OPTION_NAME].sval);
        mqttclient.subscribe(og.options[OPTION_NAME].sval +"/IN/#");
        DEBUG_PRINTLN("Subscribed to MQTT Topic");
        return true;
      }else {
        DEBUG_PRINTLN("  Failed to Connect to MQTT");
        mqtt_subscribe_timeout = curr_utc_time + 35; //Takes about 5 seconds to get through the loop
        return false;
      }
    }
  }
}

void perform_notify(String s) {
  // Blynk notification
  if(curr_cloud_access_en && Blynk.connected()) {
    Blynk.notify(s);
  }

  // IFTTT notification
  if(og.options[OPTION_IFTT].sval.length()>7) { // key size is at least 8
    DEBUG_PRINTLN("Sending IFTTT Notification");
    http.begin("http://maker.ifttt.com/trigger/opengarage/with/key/"+og.options[OPTION_IFTT].sval);
    http.addHeader("Content-Type", "application/json");
    http.POST("{\"value1\":\""+s+"\"}");
    String payload = http.getString();
    http.end();
    if(payload.indexOf("Congratulations") >= 0) {
      DEBUG_PRINTLN(" Successfully updated IFTTT");
    }else{
      DEBUG_PRINT(" Error from IFTTT: ");
      DEBUG_PRINTLN(payload);
    }
  }

  //Mqtt notification
  if(og.options[OPTION_MQTT].sval.length()>8) {
    if (mqttclient.connected()) {
        DEBUG_PRINTLN("Sending MQTT Notification");
        mqttclient.publish(og.options[OPTION_NAME].sval + "/OUT/NOTIFY",s); 
    }
  }
}

void perform_automation(byte event) {
  static bool automationclose_lock=false;
  byte ato = og.options[OPTION_ATO].ival;
  byte atob = og.options[OPTION_ATOB].ival;
  if(!ato&&!atob) {
    justopen_timestamp = 0;
    return;
  }
  if(event == DOOR_STATUS_JUST_OPENED) {
    justopen_timestamp = curr_utc_time; // record time stamp
    //perform_notify(og.options[OPTION_NAME].sval + " just OPENED!");
  } else if (event == DOOR_STATUS_JUST_CLOSED) {
    justopen_timestamp = 0; // reset time stamp
    //perform_notify(og.options[OPTION_NAME].sval + " just closed!");
  } else if (event == DOOR_STATUS_REMAIN_OPEN) {
    if (!justopen_timestamp) justopen_timestamp = curr_utc_time; // record time stamp
    else {
      if(curr_utc_time > justopen_timestamp + (ulong)og.options[OPTION_ATI].ival*60L) {
        // reached timeout, perform action
        if(ato & OG_AUTO_NOTIFY) {
          // send notification
          String s = og.options[OPTION_NAME].sval+" is left open for more than ";
          s+= og.options[OPTION_ATI].ival;
          s+= " minutes.";
          if(ato & OG_AUTO_CLOSE) {
            s+= " It will be auto-closed shortly";
          } else {
            s+= " This is a reminder for you.";
          }
          perform_notify(s);
        }
        if(ato & OG_AUTO_CLOSE) {
          // auto close door
          // alarm is mandatory in auto-close
          if(!og.options[OPTION_ALM].ival) { og.set_alarm(OG_ALM_5); }
          else { og.set_alarm(); }
        }
        justopen_timestamp = 0;
      }
      
      if( curr_utc_hour == og.options[OPTION_ATIB].ival) {
        // still open past time, perform action
        DEBUG_PRINT("Door is open at specified close time: ");
        DEBUG_PRINTLN(curr_utc_hour);
        DEBUG_PRINTLN(og.options[OPTION_ATIB].ival);
        if (automationclose_lock)
          {DEBUG_PRINTLN("  Door has already been closed once, ignore");}
        else {
          DEBUG_PRINTLN("  Door has not been closed yet");
          automationclose_lock=true;
          if(atob & OG_AUTO_NOTIFY) {
            // send notification
            String s = og.options[OPTION_NAME].sval+" is open after ";
            s+= og.options[OPTION_ATIB].ival;
            s+= " UTC. Current hour:";
            s+= curr_utc_hour;
            if(atob & OG_AUTO_CLOSE) {
              s+= " It will be auto-closed shortly";
            } else {
              s+= " This is a reminder for you.";
            }
            perform_notify(s);
          }
          if(atob & OG_AUTO_CLOSE) {
            // auto close door
            // alarm is mandatory in auto-close
            if(!og.options[OPTION_ALM].ival) { og.set_alarm(OG_ALM_5); }
            else { 
              og.set_alarm(); 
            }
          }
          justopen_timestamp = 0;
        }
      }
      else if ((curr_utc_hour > og.options[OPTION_ATIB].ival) && (automationclose_lock))
      {
        DEBUG_PRINTLN("Unlocking automation close function");
        automationclose_lock=false; //Unlock the hour after the setting
      }

    }
  } else {
    justopen_timestamp = 0;
  }
}

void check_status() {
  static ulong checkstatus_timeout = 0;
  static ulong checkstatus_report_timeout = 0;
  if(curr_utc_time > checkstatus_timeout) {
    og.set_led(HIGH);
    distance = og.read_distance();
    og.set_led(LOW);
    read_cnt = (read_cnt+1)%100;
    uint threshold = og.options[OPTION_DTH].ival;
    door_status = (distance>threshold)?0:1;
    if (og.options[OPTION_MNT].ival == OG_MNT_SIDE)    
      door_status = 1-door_status;  // reverse logic for side mount

    door_status_hist = (door_status_hist<<1) | door_status;
    byte event = check_door_status_hist();

    //Upon change
    if(event == DOOR_STATUS_JUST_OPENED || event == DOOR_STATUS_JUST_CLOSED) {

      DEBUG_PRINT(curr_utc_time);
      if(event == DOOR_STATUS_JUST_OPENED)  {	
        DEBUG_PRINTLN(" Sending State Change event to connected systems, value: DOOR_STATUS_JUST_OPENED"); }
      else if(event == DOOR_STATUS_JUST_CLOSED) {	
        DEBUG_PRINTLN(" Sending State Change event to connected systems, value: DOOR_STATUS_JUST_CLOSED"); }
      else {
        DEBUG_PRINTLN(" Sending State Change event to connected systems, value: OTHER"); 
        DEBUG_PRINTLN(String(event,DEC));
      }

      // write log record
      DEBUG_PRINTLN(" Update Local Log"); 
      LogStruct l;
      l.tstamp = curr_utc_time;
      l.status = door_status;
      l.dist = distance;
      og.write_log(l);
      
      // IFTTT notification
      if(og.options[OPTION_IFTT].sval.length()>7) { // key size is at least 8
        DEBUG_PRINTLN(" Notify IFTTT (State Change)"); 
        http.begin("http://maker.ifttt.com/trigger/opengarage/with/key/"+og.options[OPTION_IFTT].sval);
        http.addHeader("Content-Type", "application/json");
        http.POST("{\"value1\":\""+String(event,DEC)+"\"}");
        String payload = http.getString();
        http.end();
        if(payload.indexOf("Congratulations") >= 0) {
          DEBUG_PRINTLN("  Successfully updated IFTTT");
        }else{
          DEBUG_PRINT("  ERROR from IFTTT: ");
          DEBUG_PRINTLN(payload);
        }
      }

      //Mqtt notification
      if(og.options[OPTION_MQTT].sval.length()>8) {
        if (mqttclient.connected()) {
          DEBUG_PRINTLN(" Update MQTT (State Change)");
          mqttclient.publish(og.options[OPTION_NAME].sval + "/OUT/CHANGE",String(event,DEC)); 
        }
      }

    } //End state change updates

    //Send current status only on change and longer interval
    if ((curr_utc_time >checkstatus_report_timeout) || (event == DOOR_STATUS_JUST_OPENED || event == DOOR_STATUS_JUST_CLOSED) ){
      DEBUG_PRINT(curr_utc_time);
      if(event == DOOR_STATUS_REMAIN_OPEN)  {	
        DEBUG_PRINTLN(" Sending State Refresh to connected systems, value: OPEN"); }
      else if(event == DOOR_STATUS_REMAIN_CLOSED) {	
        DEBUG_PRINTLN(" Sending State Refresh to connected systems, value: CLOSED"); }

      // report status to Blynk
      if(curr_cloud_access_en && Blynk.connected()) {
        DEBUG_PRINTLN(" Update Blynk (Status Refresh)");
        Blynk.virtualWrite(BLYNK_PIN_RCNT, read_cnt);
        Blynk.virtualWrite(BLYNK_PIN_DIST, distance);
        (door_status) ? blynk_led.on() : blynk_led.off();
        Blynk.virtualWrite(BLYNK_PIN_IP, get_ip());
        blynk_lcd.print(0, 0, get_ip());
        String str = ":";
        str += og.options[OPTION_HTP].ival;
        str += " " + get_ap_ssid();
        blynk_lcd.print(0, 1, str);
      }
      
      //IFTTT only recieves state change events not ongoing status

      //Mqtt notification
      if((og.options[OPTION_MQTT].sval.length()>8) && (mqttclient.connected())) {
        DEBUG_PRINTLN(" Update MQTT (Status Refresh)");
        if(door_status == DOOR_STATUS_REMAIN_OPEN)  {						// MQTT: If door open...
          mqttclient.publish(og.options[OPTION_NAME].sval + "/OUT/STATE","OPEN");
          mqttclient.publish(og.options[OPTION_NAME].sval,"Open"); //Support existing mqtt code
          //DEBUG_PRINTLN(curr_utc_time + " Sending MQTT State otification: OPEN");
        } 
        else if(door_status == DOOR_STATUS_REMAIN_CLOSED) {					// MQTT: If door closed...
          mqttclient.publish(og.options[OPTION_NAME].sval + "/OUT/STATE","CLOSED");
          mqttclient.publish(og.options[OPTION_NAME].sval,"Closed"); //Support existing mqtt code
          //DEBUG_PRINTLN(curr_utc_time + " Sending MQTT State Notification: CLOSED");
        }
      }
      //Set to run every 5 minutes -no need to continually send status when changes drive it
      checkstatus_report_timeout= curr_utc_time + 120L; 
    }
    
    //Process any built in automations
    perform_automation(event);
    checkstatus_timeout = curr_utc_time + og.options[OPTION_RIV].ival;
    
  }
}

void time_keeping() {
  static bool configured = false;
  static ulong prev_millis = 0;
  static ulong time_keeping_timeout = 0;

  if(!configured) {
    DEBUG_PRINTLN(F("set time server"));
    configTime(0, 0, "pool.ntp.org", "time.nist.org", NULL);
    configured = true;
  }

  if(!curr_utc_time || (curr_utc_time > time_keeping_timeout)) {
    ulong gt = time(nullptr);
    if(!gt) {
      // if we didn't get response, re-try after 2 seconds
      time_keeping_timeout = curr_utc_time + 2;
    } else {
      curr_utc_time = gt;
      curr_utc_hour = (curr_utc_time % 86400)/3600;
      DEBUG_PRINT(F("Updated time from NTP: "));
      DEBUG_PRINT(curr_utc_time);
      DEBUG_PRINT(" Hour: ");
      DEBUG_PRINTLN(curr_utc_hour);
      // if we got a response, re-try after TIME_SYNC_TIMEOUT seconds
      time_keeping_timeout = curr_utc_time + TIME_SYNC_TIMEOUT;
      prev_millis = millis();
    }
  }
  while(millis() - prev_millis >= 1000) {
    curr_utc_time ++;
    prev_millis += 1000;
  }
}

void process_alarm() {
  if(!og.alarm) return;
  static ulong prev_half_sec = 0;
  ulong curr_half_sec = millis()/500;
  if(curr_half_sec != prev_half_sec) {  
    prev_half_sec = curr_half_sec;
    if(prev_half_sec % 2 == 0) {
      og.play_note(ALARM_FREQ);
    } else {
      og.play_note(0);
    }
    og.alarm--;
    if(og.alarm==0) {
      og.play_note(0);
      og.click_relay();
    }
  }
}


void do_loop() {

  static ulong connecting_timeout;
  
  switch(og.state) {
  case OG_STATE_INITIAL:
    if(curr_mode == OG_MOD_AP) {
      scanned_ssids = scan_network();
      String ap_ssid = get_ap_ssid();
      start_network_ap(ap_ssid.c_str(), NULL);
      server->on("/",   on_home);    
      server->on("/js", on_ap_scan);
      server->on("/cc", on_ap_change_config);
      server->on("/jt", on_ap_try_connect);
      server->begin();
      og.state = OG_STATE_CONNECTED;
      DEBUG_PRINTLN(WiFi.softAPIP());
    } else {
      led_blink_ms = LED_SLOW_BLINK;
      start_network_sta(og.options[OPTION_SSID].sval.c_str(), og.options[OPTION_PASS].sval.c_str());
      og.config_ip();
      og.state = OG_STATE_CONNECTING;
      connecting_timeout = millis() + 60000;
    }
    break;

  case OG_STATE_TRY_CONNECT:
    led_blink_ms = LED_SLOW_BLINK;
    start_network_sta_with_ap(og.options[OPTION_SSID].sval.c_str(), og.options[OPTION_PASS].sval.c_str());
    og.config_ip();
    og.state = OG_STATE_CONNECTED;
    break;
    
  case OG_STATE_CONNECTING:
    if(WiFi.status() == WL_CONNECTED) {
      if(curr_local_access_en) {
        // use ap ssid as mdns name
        if(MDNS.begin(get_ap_ssid().c_str())) {
          DEBUG_PRINTLN(F("MDNS registered"));
        }
        server->on("/", on_home);
        server->on("/jc", on_sta_controller);
        server->on("/jo", on_sta_options);
        server->on("/jl", on_sta_logs);
        server->on("/vo", on_sta_view_options);
        server->on("/vl", on_sta_view_logs);
        server->on("/cc", on_sta_change_controller);
        server->on("/co", on_sta_change_options);
        server->on("/update", HTTP_GET, on_sta_update);
        server->on("/update", HTTP_POST, on_sta_upload_fin, on_sta_upload);
        server->begin();
      }
      if(curr_cloud_access_en) {
        Blynk.config(og.options[OPTION_AUTH].sval.c_str()); // use the config function
        Blynk.connect();
      }

      og.state = OG_STATE_CONNECTED;
      // save device IP and gateway information
      
      led_blink_ms = 0;
      og.set_led(LOW);
      
      DEBUG_PRINTLN(WiFi.localIP());

    } else {
      if(millis() > connecting_timeout) {
        og.state = OG_STATE_INITIAL;
        DEBUG_PRINTLN(F("timeout"));
      }
    }
    break;
  
  case OG_STATE_CONNECTED:
    if(curr_mode == OG_MOD_AP) {
      server->handleClient();
    } else {
      if(WiFi.status() == WL_CONNECTED) {
        if(curr_local_access_en)
          server->handleClient();
        if(curr_cloud_access_en)
          Blynk.run();
      } else {
        og.state = OG_STATE_INITIAL;
      }
    }
    break;
    
  case OG_STATE_RESTART:
    if(curr_local_access_en)
      server->handleClient();
    if(millis() > restart_timeout) {
      og.state = OG_STATE_INITIAL;
      og.restart();
    }
    break;
    
  case OG_STATE_RESET:
    og.state = OG_STATE_INITIAL;
    og.options_reset();
    og.log_reset();
    restart_timeout = millis();
    og.state = OG_STATE_RESTART;
    break;
  }
  
  if(og.state == OG_STATE_CONNECTED && curr_mode == OG_MOD_STA) {
    time_keeping();
    check_status();

    //Handle MQTT
    if(og.options[OPTION_MQTT].sval.length()>8) {
      if (!mqttclient.connected()) {
         mqtt_connect_subscibe();
      }
      else {mqttclient.loop();} //Processes MQTT Pings/keep alives
    }
  }
  
  if(curr_mode == OG_MOD_AP) {
    check_status_ap();
  }
  process_ui();
  if(og.alarm)
    process_alarm();
}

BLYNK_WRITE(V1)
{
  DEBUG_PRINTLN("Blynk Remote Open Command Issued");
  if(!og.options[OPTION_ALM].ival) {
    // if alarm is disabled, trigger right away
    if(param.asInt()) {
      og.set_relay(HIGH);
    } else {
      og.set_relay(LOW);
    }
  } else {
    // otherwise, set alarm
    if(param.asInt()) {
      og.set_alarm();
    }  
  }
}

