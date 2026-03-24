#include <DHT.h>
#include <Wire.h>
#include <RTClib.h>

/************************** 引脚定义（避坑：非调试/USB引脚） **************************/
#define MQ135_AO_PIN PA0       // MQ135模拟输入
#define DHT11_PIN PA7          // DHT11数据脚
#define DHT_TYPE DHT11         // DHT11类型
#define NIGHT_LED_PIN PB12     // 夜间定时灯
#define ROAST_LED_PIN PB13     // 烤灯（原温度灯，低温/高湿触发）
#define GPS23_MAG_PIN PB15     // 磁控开关检测脚（长臂常闭型）
#define RELAY_PIN PA12         // 继电器（风扇）控制引脚
#define SERIAL_TX_NO_INTERRUPT 1  // 核心绝杀：关闭STM32串口发送中断，彻底解决millis冲突导致的乱码

/************************** 核心阈值参数（按需修改！支持串口动态修改） **************************/
// 1. 烤灯触发阈值（低温/高湿任一满足）
float ROAST_TEMP_LOW = 20.0;    // 烤灯触发：温度≤20℃
float ROAST_HUMI_HIGH = 70.0;   // 烤灯触发：湿度≥70%RH
unsigned long ROAST_LED_TIME = 8000;    // 烤灯亮灯时长（8秒）

// 2. 风扇（除湿+除味）触发阈值
float FAN_HUMI_TRIG = 65.0;     // 除湿触发：湿度≥65%RH
uint16_t FAN_AIR_TRIG = 600;    // 除味触发：MQ135差值≥600
unsigned long FAN_RUN_TIME = 10000;     // 风扇运行时长（10秒）
unsigned long FAN_COOL_DOWN = 5000;     // 防频繁触发冷却时间（5秒）

// 3. 其他辅助参数
#define GPS23_DEBOUNCE 200     // 磁控开关消抖时间
int TARGET_HOUR_START = 1;     // 夜间灯开启小时
int TARGET_HOUR_END = 2;       // 夜间灯关闭小时

/************************** 空气质量枚举 **************************/
typedef enum { AIR_EXCELLENT=0, AIR_GOOD=1, AIR_NORMAL=2, AIR_BAD=3, AIR_WORSE=4 } AirQuality;

/************************** 全局变量 **************************/
DHT dht(DHT11_PIN, DHT_TYPE);
RTC_DS3231 rtc;
uint16_t mq135_base = 0;
bool is_calibrated = false;

// 磁控开关状态
bool is_gps23_magnet_near = false;
unsigned long gps23_last_check = 0;

// 烤灯状态 + 新增手动控制标记
bool is_roast_led_on = false;
unsigned long roast_led_start = 0;
bool roast_manual_mode = false;  // 烤灯手动模式标记 true=手动控制 false=自动控制

// 风扇状态 + 新增手动控制标记
bool is_fan_on = false;
unsigned long fan_start = 0;
unsigned long last_fan_trigger = 0;
bool fan_manual_mode = false;    // 风扇手动模式标记 true=手动控制 false=自动控制

// 夜间灯状态 + 新增手动控制标记
bool night_manual_mode = false;  // 夜间灯手动模式标记 true=手动控制 false=自动定时
bool is_night_led_on = false;

// 打印/串口相关
unsigned long last_print_time = 0;
String serial_recv_buf = "";     // 串口接收缓冲区 用于解析ESP8266指令

/************************** MQ135校准函数 **************************/
void MQ135_Calibrate() {
  Serial.println("\n===== MQ135校准 =====");
  Serial.println("请将传感器置于清新空气中！");
  delay(5000);
  
  uint32_t sum = 0;
  uint8_t valid_count = 0;
  for (uint8_t i = 0; i < 20; i++) {
    uint16_t val = analogRead(MQ135_AO_PIN);
    if (val > 0 && val < 4095) {
      sum += val;
      valid_count++;
    }
    Serial.print("MQ135采集值："); Serial.println(val);
    delay(500);
  }
  if (valid_count == 0) {
    Serial.println("【校准失败】无有效采样值！");
    return;
  }
  mq135_base = sum / valid_count;
  
  if (mq135_base > 4000) {
    Serial.println("【校准异常】值过高，检查传感器供电！");
  } else {
    is_calibrated = true;
    Serial.print("校准完成，基准值："); Serial.println(mq135_base);
  }
  delay(2000);
}

/************************** 状态判断函数 **************************/
// 烤灯触发判断：返回true=满足（低温/高湿）
bool Is_Roast_Trigger(float temp, float humi) {
  if (isnan(temp) || isnan(humi)) return false;
  return (temp <= ROAST_TEMP_LOW) || (humi >= ROAST_HUMI_HIGH);
}

// 风扇触发判断：返回true=满足（高湿/空气质量差）
bool Is_Fan_Trigger(float humi, uint16_t mq135_val) {
  if (!is_calibrated || isnan(humi)) return false;
  uint16_t air_diff = abs(mq135_val - mq135_base);
  return (humi >= FAN_HUMI_TRIG) || (air_diff >= FAN_AIR_TRIG);
}

// 空气质量等级判断（仅打印用）
AirQuality Get_AirQuality(uint16_t mq135_val) {
  if (!is_calibrated) return AIR_WORSE;
  uint16_t diff = abs(mq135_val - mq135_base);
  if (diff < 50) return AIR_EXCELLENT;
  else if (diff < 200) return AIR_GOOD;
  else if (diff < 400) return AIR_NORMAL;
  else if (diff < 800) return AIR_BAD;
  else return AIR_WORSE;
}

/************************** DHT11读取函数（增强重试） **************************/
bool Read_DHT11(float &humi, float &temp) {
  humi = dht.readHumidity();
  temp = dht.readTemperature();
  if (!isnan(humi) && !isnan(temp)) return true;
  
  for (uint8_t i = 0; i < 5; i++) {
    delay(200);
    humi = dht.readHumidity();
    temp = dht.readTemperature();
    if (!isnan(humi) && !isnan(temp)) return true;
  }
  Serial.println("【DHT11】读取失败！");
  return false;
}

/************************** GPS23磁控开关检测（增强消抖） **************************/
void Check_GPS23() {
  if (millis() - gps23_last_check < GPS23_DEBOUNCE) return;
  gps23_last_check = millis();
  
  uint8_t high_count = 0;
  for (uint8_t i=0; i<5; i++) {
    if (digitalRead(GPS23_MAG_PIN) == HIGH) high_count++;
    delay(10);
  }
  is_gps23_magnet_near = (high_count >= 3);
  
  // 磁铁远离→强制关闭所有设备 + 清除手动模式
  if (!is_gps23_magnet_near) {
    digitalWrite(ROAST_LED_PIN, LOW);
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(NIGHT_LED_PIN, LOW);
    
    is_roast_led_on = false;
    is_fan_on = false;
    is_night_led_on = false;
    roast_manual_mode = false;
    fan_manual_mode = false;
    night_manual_mode = false;
    Serial.println("【GPS23】磁铁远离，关闭所有设备");
  }
}

/************************** 烤灯控制（新增手动优先+自动逻辑） **************************/
void Control_Roast_LED(bool trigger) {
  if (!is_gps23_magnet_near) {
    digitalWrite(ROAST_LED_PIN, LOW);
    is_roast_led_on = false;
    roast_manual_mode = false;
    return;
  }

  // ✅ 手动模式优先级最高：手动模式下，不执行自动逻辑
  if(roast_manual_mode){
    return;
  }

  // 自动模式逻辑（原有逻辑不变）
  if (trigger && !is_roast_led_on) {
    digitalWrite(ROAST_LED_PIN, HIGH);
    is_roast_led_on = true;
    roast_led_start = millis();
    Serial.println("【烤灯】低温/高湿触发，灯亮");
  }
  else if (is_roast_led_on) {
    if (millis() - roast_led_start >= ROAST_LED_TIME) {
      digitalWrite(ROAST_LED_PIN, LOW);
      is_roast_led_on = false;
      Serial.println("【烤灯】时长到，熄灭");
    }
  }
  else {
    digitalWrite(ROAST_LED_PIN, LOW);
  }
}

/************************** 风扇控制（新增手动优先+自动逻辑） **************************/
void Control_Fan(bool trigger) {
  if (!is_gps23_magnet_near) {
    digitalWrite(RELAY_PIN, LOW);
    is_fan_on = false;
    fan_manual_mode = false;
    return;
  }

  // ✅ 手动模式优先级最高：手动模式下，不执行自动逻辑+跳过冷却时间判断
  if(fan_manual_mode){
    return;
  }

  // 冷却时间：5秒内不重复触发
  if (millis() - last_fan_trigger < FAN_COOL_DOWN) return;

  // 自动模式逻辑（原有逻辑不变）
  if (trigger && !is_fan_on) {
    digitalWrite(RELAY_PIN, HIGH);
    is_fan_on = true;
    fan_start = millis();
    last_fan_trigger = millis();
    Serial.println("【风扇】高湿/异味触发，启动");
  }
  else if (is_fan_on) {
    if (millis() - fan_start >= FAN_RUN_TIME) {
      digitalWrite(RELAY_PIN, LOW);
      is_fan_on = false;
      Serial.println("【风扇】运行时长到，停止");
    }
  }
  else {
    digitalWrite(RELAY_PIN, LOW);
  }
}

/************************** 夜间灯控制（新增手动优先+自动定时逻辑） **************************/
void Control_Night_LED() {
  if (!is_gps23_magnet_near) {
    digitalWrite(NIGHT_LED_PIN, LOW);
    is_night_led_on = false;
    night_manual_mode = false;
    return;
  }

  // ✅ 手动模式优先级最高：手动模式下，不执行自动定时逻辑
  if(night_manual_mode){
    return;
  }

  // 自动定时模式（原有逻辑不变）
  int hour = rtc.now().hour();
  is_night_led_on = (hour >= TARGET_HOUR_START && hour < TARGET_HOUR_END);
  digitalWrite(NIGHT_LED_PIN, is_night_led_on ? HIGH : LOW);
}

/************************** ✨ 新增核心函数：串口指令解析（匹配ESP8266所有指令） **************************/
void Parse_Serial_Cmd() {
  // 串口有数据则读取到缓冲区
  while(Serial.available() > 0){
    char c = Serial.read();
    if(c == '\n'){ // 换行符为指令结束符
      serial_recv_buf.trim();
      if(serial_recv_buf.length() > 0){
        // ===== 1. 烤灯手动指令 =====
        if(serial_recv_buf == "ROAST_LED_ON"){
          if(is_gps23_magnet_near){
            digitalWrite(ROAST_LED_PIN, HIGH);
            is_roast_led_on = true;
            roast_led_start = millis();
            roast_manual_mode = true;
            Serial.println("【烤灯】手动开启");
          }
        }
        else if(serial_recv_buf == "ROAST_LED_OFF"){
          digitalWrite(ROAST_LED_PIN, LOW);
          is_roast_led_on = false;
          roast_manual_mode = false;
          Serial.println("【烤灯】手动关闭");
        }
        // 修改烤灯运行时长
        else if(serial_recv_buf.startsWith("ROAST_TIME=")){
          ROAST_LED_TIME = serial_recv_buf.substring(10).toInt();
          Serial.print("【烤灯】时长修改为：");Serial.println(ROAST_LED_TIME);
        }

        // ===== 2. 风扇手动指令 =====
        else if(serial_recv_buf == "FAN_ON"){
          if(is_gps23_magnet_near){
            digitalWrite(RELAY_PIN, HIGH);
            is_fan_on = true;
            fan_start = millis();
            fan_manual_mode = true;
            Serial.println("【风扇】手动开启");
          }
        }
        else if(serial_recv_buf == "FAN_OFF"){
          digitalWrite(RELAY_PIN, LOW);
          is_fan_on = false;
          fan_manual_mode = false;
          Serial.println("【风扇】手动关闭");
        }
        // 修改风扇运行时长
        else if(serial_recv_buf.startsWith("FAN_TIME=")){
          FAN_RUN_TIME = serial_recv_buf.substring(8).toInt();
          Serial.print("【风扇】时长修改为：");Serial.println(FAN_RUN_TIME);
        }

        // ===== 3. 夜间灯手动指令 =====
        else if(serial_recv_buf == "NIGHT_LED_ON"){
          if(is_gps23_magnet_near){
            digitalWrite(NIGHT_LED_PIN, HIGH);
            is_night_led_on = true;
            night_manual_mode = true;
            Serial.println("【夜间灯】手动开启");
          }
        }
        else if(serial_recv_buf == "NIGHT_LED_OFF"){
          digitalWrite(NIGHT_LED_PIN, LOW);
          is_night_led_on = false;
          night_manual_mode = false;
          Serial.println("【夜间灯】手动关闭");
        }
        // 修改夜间灯开启小时
        else if(serial_recv_buf.startsWith("NIGHT_START=")){
          TARGET_HOUR_START = serial_recv_buf.substring(11).toInt();
          Serial.print("【夜间灯】开启时间修改为：");Serial.println(TARGET_HOUR_START);
        }
        // 修改夜间灯关闭小时
        else if(serial_recv_buf.startsWith("NIGHT_END=")){
          TARGET_HOUR_END = serial_recv_buf.substring(9).toInt();
          Serial.print("【夜间灯】关闭时间修改为：");Serial.println(TARGET_HOUR_END);
        }
      }
      serial_recv_buf = ""; // 清空缓冲区
    }
    else if(c != '\r'){ // 过滤回车符
      serial_recv_buf += c;
    }
  }
}

/************************** 信息打印函数（非阻塞） **************************/
/************************** 信息打印函数（非阻塞，无中文，彻底解决乱码） **************************/
/************************** 信息打印函数（绝杀乱码版：纯数字+极简分隔符，无任何特殊字符） **************************/
void Print_Info() {
  if (millis() - last_print_time < 1000) return;
  last_print_time = millis();
  
  uint16_t mq135_val = analogRead(MQ135_AO_PIN);
  float humi = 0, temp = 0;
  bool dht_ok = Read_DHT11(humi, temp);
  uint16_t air_diff = abs(mq135_val - mq135_base);

  // 执行所有控制逻辑，原逻辑不变
  Check_GPS23();
  Control_Night_LED();
  Control_Roast_LED(Is_Roast_Trigger(temp, humi));
  Control_Fan(Is_Fan_Trigger(humi, mq135_val));
  
  // ============ ✅ 绝杀乱码核心格式：纯数字 + 英文逗号分隔 【一行到底，无任何特殊字符，无断行】 ============
  // 数据格式固定死：时,分,秒,湿度,温度,是否温湿度正常(1=正常0=失败),MQ135值,MQ135差值,烤灯状态(1=开0=关),风扇状态(1=开0=关),磁控状态(1=靠近0=远离)
  DateTime now = rtc.now();
  Serial.print(now.hour());Serial.print(",");
  Serial.print(now.minute());Serial.print(",");
  Serial.print(now.second());Serial.print(",");
  Serial.print(dht_ok ? humi : 0);Serial.print(",");
  Serial.print(dht_ok ? temp : 0);Serial.print(",");
  Serial.print(dht_ok ? 1 : 0);Serial.print(",");
  Serial.print(mq135_val);Serial.print(",");
  Serial.print(air_diff);Serial.print(",");
  Serial.print(is_roast_led_on ? 1 : 0);Serial.print(",");
  Serial.print(is_fan_on ? 1 : 0);Serial.print(",");
  Serial.print(is_gps23_magnet_near ? 1 : 0);
  Serial.println(); // 只有最后一个换行符，无任何多余字符
}
/************************** 初始化函数 **************************/
void setup() {
  // 引脚初始化
  pinMode(MQ135_AO_PIN, INPUT);
  pinMode(DHT11_PIN, INPUT);
  pinMode(NIGHT_LED_PIN, OUTPUT);
  pinMode(ROAST_LED_PIN, OUTPUT);
  pinMode(GPS23_MAG_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT); 
  
  // 初始状态：所有输出低电平
  digitalWrite(NIGHT_LED_PIN, LOW);
  digitalWrite(ROAST_LED_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW); 
  
  // 串口初始化 (波特率9600 与ESP8266完全匹配)
  Serial.begin(9600, SERIAL_8N1);
  while(!Serial);
  
  // DS3231初始化（弱化阻塞）
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("❌ DS3231未检测到！跳过RTC功能");
  } else {
    if (rtc.lostPower()) {
      Serial.println("⚠️ DS3231掉电，设置默认时间");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
  
  // DHT11初始化
  dht.begin();
  delay(3000);
  
  // 启动信息
  Serial.println("============================");
  Serial.println(" 烤灯+风扇（除湿除味）控制 ");
  Serial.print(" 烤灯触发：≤"); Serial.print(ROAST_TEMP_LOW); Serial.print("℃ 或 ≥"); Serial.print(ROAST_HUMI_HIGH); Serial.println("%RH");
  Serial.print(" 风扇触发：≥"); Serial.print(FAN_HUMI_TRIG); Serial.print("%RH 或 MQ135差值≥"); Serial.print(FAN_AIR_TRIG);
  Serial.println("✅ 已加入手动控制功能 + ESP8266串口指令解析");
  Serial.println("============================");
  
  // MQ135校准
  MQ135_Calibrate();
}

/************************** 主循环 **************************/
void loop() {
  Parse_Serial_Cmd();  // ✅ 轮询解析串口指令（核心新增）
  Print_Info();
  delay(200); // 非阻塞短延时，保证响应速度
}