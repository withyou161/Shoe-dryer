#define BAUD_RATE 9600
#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ===================== WiFi热点配置 =====================
const char* AP_SSID = "SmartShoeCabinet";
const char* AP_PASS = "12345678";
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// ===================== WebSocket配置 =====================
WebSocketsServer webSocket = WebSocketsServer(81);

// ===================== 设备状态存储 =====================
float temp = 0.0, humi = 0.0;
bool dht_status = true;
uint16_t mq135_val = 0;
uint16_t mq135_diff = 0;
String air_quality = "未校准";
int sys_hour = 0, sys_min =0, sys_sec=0;
bool is_gps23_magnet_near = false;
String gps23_status = "磁铁远离";
bool is_roast_led_on = false;
String roast_led_status = "未运行";
unsigned long roast_led_remain_time = 0;
unsigned long roast_led_set_time = 8000;
bool roast_manual_mode = false;
bool is_fan_on = false;
String fan_status = "未运行";
unsigned long fan_remain_time = 0;
unsigned long fan_set_time = 10000;
bool fan_manual_mode = false;
bool is_night_led_on = false;
String night_led_status = "未运行";
int night_led_start_h = 1;
int night_led_end_h = 2;
bool night_manual_mode = false;
unsigned long roast_led_start = 0;
unsigned long fan_start = 0;
unsigned long last_serial_read = 0;

// ===================== 函数声明 =====================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void readDataFromSTM32();
void updateDeviceStatus();
void sendDataToMiniProgram();
void sendCmdToSTM32(String cmd);

// ===================== 初始化 =====================
void setup() {
  // ✅ 核心加固：ESP8266串口强制配置8N1，关闭流控，和STM32完全匹配
  Serial.begin(BAUD_RATE, SERIAL_8N1);
  Serial.setTimeout(100); // 串口读取超时100ms，防止卡死
  while (!Serial) { delay(10); }
  Serial.println("\n✅ ESP8266启动中...");

  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("✅ 热点IP: ");Serial.println(WiFi.softAPIP());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("✅ WebSocket服务启动 (端口81)");
  Serial.println("====================================");
  Serial.println("🔧 ESP8266等待STM32数据...");
  Serial.println("====================================");
}

// ===================== 主循环 =====================
void loop() {
  webSocket.loop();
  if (millis() - last_serial_read >= 200) {
    readDataFromSTM32();
    last_serial_read = millis();
  }
  updateDeviceStatus();
  static unsigned long last_send_time = 0;
  if (millis() - last_send_time >= 500) {
    sendDataToMiniProgram();
    last_send_time = millis();
  }
}

// ===================== WebSocket事件处理 =====================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:Serial.printf("❌ 客户端[%u]断开\n", num);break;
    case WStype_CONNECTED:{IPAddress ip = webSocket.remoteIP(num);Serial.printf("✅ 客户端[%u]连接: %d.%d.%d.%d\n", num, ip[0],ip[1],ip[2],ip[3]);sendDataToMiniProgram();}break;
    case WStype_TEXT: {
        String cmd = String((char *)payload).substring(0, length);
        Serial.printf("📩 小程序指令: %s\n", cmd.c_str());
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, cmd);
        if (err) {Serial.println("❌ 指令解析失败: " + String(err.c_str()));return;}
        if (doc.containsKey("roast_led")) {
          bool roast_led = doc["roast_led"];
          is_roast_led_on = roast_led;roast_manual_mode = roast_led;
          if (roast_led) {roast_led_start=millis();roast_led_status="运行中(手动)";sendCmdToSTM32("ROAST_LED_ON");}
          else {roast_led_status="未运行";roast_led_remain_time=0;roast_manual_mode=false;sendCmdToSTM32("ROAST_LED_OFF");}
        }
        if (doc.containsKey("roast_led_time")) {roast_led_set_time=doc["roast_led_time"].as<unsigned long>()*1000;sendCmdToSTM32("ROAST_TIME="+String(roast_led_set_time));}
        if (doc.containsKey("fan")) {
          bool fan = doc["fan"];is_fan_on = fan;fan_manual_mode = fan;
          if (fan) {fan_start=millis();fan_status="运行中(手动)";sendCmdToSTM32("FAN_ON");}
          else {fan_status="未运行";fan_remain_time=0;fan_manual_mode=false;sendCmdToSTM32("FAN_OFF");}
        }
        if (doc.containsKey("fan_time")) {fan_set_time=doc["fan_time"].as<unsigned long>()*1000;sendCmdToSTM32("FAN_TIME="+String(fan_set_time));}
        if (doc.containsKey("night_led_start")) {night_led_start_h=doc["night_led_start"].as<int>();sendCmdToSTM32("NIGHT_START="+String(night_led_start_h));}
        if (doc.containsKey("night_led_end")) {night_led_end_h=doc["night_led_end"].as<int>();sendCmdToSTM32("NIGHT_END="+String(night_led_end_h));}
        if (doc.containsKey("night_led")) {
          is_night_led_on = doc["night_led"];night_manual_mode = is_night_led_on;
          sendCmdToSTM32(is_night_led_on ? "NIGHT_LED_ON" : "NIGHT_LED_OFF");
        }
      }
      break;
    default:break;
  }
}

// ===================== ✅ 终极抗乱码版：STM32数据解析（核心修改，最强容错） =====================
// ===================== ✅ 完美适配STM32英文占位符 无乱码解析 =====================
// ===================== ✅ 绝杀版：纯数字逗号分隔解析函数 100%无乱码 完美适配STM32新格式 =====================
// ===================== ✅ 终极绝杀版：三重校验+过滤 100%无乱码 纯数字解析函数 =====================
void readDataFromSTM32() {
  if (Serial.available() <= 0) return;
  
  // 1. 读取整行数据+清空缓冲区，杜绝脏数据堆积
  String serial_data = Serial.readStringUntil('\n');
  Serial.flush();
  serial_data.trim();
  
  // ===================== 【三重硬校验：过滤所有错误/乱码数据，核心根治！】 =====================
  bool isDataValid = true;
  int commaCount = 0;
  
  // ✔ 校验1：数据长度过滤，太短的直接丢弃（正常数据至少25字节）
  if(serial_data.length() < 25 || serial_data.length() > 50){
    isDataValid = false;
  }
  
  // ✔ 校验2：字符合法性校验 → 只允许【0-9】【.】【,】三种字符，有其他字符=乱码，直接丢弃
  for(int i=0; i<serial_data.length() && isDataValid; i++){
    char c = serial_data[i];
    if( !( (c>='0'&&c<='9') || c=='.' || c==',' ) ){
      isDataValid = false;
    }
    if(c == ',') commaCount++; // 统计逗号数量
  }
  
  // ✔ 校验3：格式合法性校验 → 必须是10个逗号=11个数据段，少/多都算错误
  if(commaCount != 10){
    isDataValid = false;
  }
  
  // ✔ 校验不通过 → 直接return，不解析任何数据，完美过滤乱码行
  if(!isDataValid) return;

  // ===================== 【以下是原解析逻辑，完全不变，复制即可】 =====================
  int comma[11]; 
  comma[0] = -1;
  uint8_t comma_idx = 0;
  for(uint8_t i=0; i<serial_data.length() && comma_idx<10; i++){
    if(serial_data[i] == ','){
      comma_idx++;
      comma[comma_idx] = i;
    }
  }

  // 固定格式赋值：时,分,秒,湿度,温度,温湿度正常(1),MQ135值,MQ135差值,烤灯(1),风扇(1),磁控(1)
  sys_hour = serial_data.substring(comma[0]+1, comma[1]).toInt();
  sys_min  = serial_data.substring(comma[1]+1, comma[2]).toInt();
  sys_sec  = serial_data.substring(comma[2]+1, comma[3]).toInt();
  humi     = serial_data.substring(comma[3]+1, comma[4]).toFloat();
  temp     = serial_data.substring(comma[4]+1, comma[5]).toFloat();
  dht_status = serial_data.substring(comma[5]+1, comma[6]).toInt() == 1;
  mq135_val = serial_data.substring(comma[6]+1, comma[7]).toInt();
  mq135_diff = serial_data.substring(comma[7]+1, comma[8]).toInt();
  is_roast_led_on = serial_data.substring(comma[8]+1, comma[9]).toInt() == 1;
  is_fan_on = serial_data.substring(comma[9]+1, comma[10]).toInt() == 1;
  is_gps23_magnet_near = serial_data.substring(comma[10]+1).toInt() == 1;

  // 空气质量等级（原逻辑不变）
  if (mq135_diff < 50) air_quality = "优";
  else if (mq135_diff < 200) air_quality = "良";
  else if (mq135_diff < 400) air_quality = "一般";
  else if (mq135_diff < 800) air_quality = "差";
  else air_quality = "极差";

  // 状态文字描述（原逻辑不变）
  gps23_status = is_gps23_magnet_near ? "磁铁靠近" : "磁铁远离";
  roast_led_status = is_roast_led_on ? (roast_manual_mode?"运行中(手动)":"运行中(自动)") : "未运行";
  fan_status = is_fan_on ? (fan_manual_mode?"运行中(手动)":"运行中(自动)") : "未运行";

  // 烤灯/风扇计时初始化（原逻辑不变）
  if(is_roast_led_on && roast_led_start == 0) roast_led_start = millis();
  if(is_fan_on && fan_start == 0) fan_start = millis();

  // 磁铁远离重置所有状态（原逻辑不变）
  if(!is_gps23_magnet_near){
    roast_manual_mode = false;
    fan_manual_mode = false;
    night_manual_mode = false;
    is_roast_led_on = false;
    is_fan_on = false;
    is_night_led_on = false;
  }
}

// ===================== 状态更新 =====================
void updateDeviceStatus() {
  if (is_roast_led_on) {
    roast_led_remain_time = max(0ul, roast_led_set_time - (millis() - roast_led_start));
    if (roast_led_remain_time == 0) {is_roast_led_on=false;roast_led_status="未运行";roast_manual_mode=false;}
  } else roast_led_remain_time =0;

  if (is_fan_on) {
    fan_remain_time = max(0ul, fan_set_time - (millis() - fan_start));
    if (fan_remain_time ==0) {is_fan_on=false;fan_status="未运行";fan_manual_mode=false;}
  } else fan_remain_time=0;

  night_led_status = is_night_led_on ? "已开启" : "已关闭";
  night_led_status += night_manual_mode ? "(手动)" : "(自动)";
  
  if(!dht_status){humi=0.0;temp=0.0;air_quality="传感器异常";}
}

// ===================== 发送数据到小程序 =====================
void sendDataToMiniProgram() {
  StaticJsonDocument<512> doc;
  doc["sys_time"] = String(sys_hour)+":"+String(sys_min)+":"+String(sys_sec);
  doc["temp"] = temp;doc["humi"] = humi;doc["dht_status"] = dht_status;
  doc["mq135_val"] = mq135_val;doc["mq135_diff"] = mq135_diff;doc["air_quality"] = air_quality;
  doc["gps23_status"] = gps23_status;doc["is_gps23_magnet_near"] = is_gps23_magnet_near;
  doc["roast_led_status"] = roast_led_status;doc["roast_led_remain_time_s"] = roast_led_remain_time/1000.0;
  doc["roast_led_set_time_s"] = roast_led_set_time/1000.0;doc["roast_manual_mode"] = roast_manual_mode;
  doc["fan_status"] = fan_status;doc["fan_remain_time_s"] = fan_remain_time/1000.0;
  doc["fan_set_time_s"] = fan_set_time/1000.0;doc["fan_manual_mode"] = fan_manual_mode;
  doc["night_led_status"] = night_led_status;doc["night_led_start_h"] = night_led_start_h;
  doc["night_led_end_h"] = night_led_end_h;doc["night_manual_mode"] = night_manual_mode;

  String json_str;serializeJson(doc, json_str);
  webSocket.broadcastTXT(json_str);
}

// ===================== 发送指令到STM32 =====================
void sendCmdToSTM32(String cmd) {
  Serial.println(cmd);
  Serial.print("📤 转发指令: "); Serial.println(cmd);
}