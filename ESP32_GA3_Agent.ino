/*******************************************************************************
 * ESP32 USB 통신 진단 + WiFi + MQTT 전송 (v4 - TEST MODE v2)
 * 
 * ============================================================================
 * TEST_MODE 배선 구성:
 * ============================================================================
 * 
 *   [Serial Port Utility]          [Arduino Serial Monitor]
 *        COM3                             COM9
 *         |                                |
 *    USB-UART 어댑터                   ESP32 USB
 *     (CP2102/CH340)                      |
 *         |                               |
 *    TX ──→ GPIO16 (RX2)          Serial (디버그 출력)
 *    RX ←── GPIO17 (TX2)          
 *    GND ── GND                   
 *         |                               
 *      Serial2                        
 *    (패킷 수신/ACK 전송)         
 * 
 * ============================================================================
 * 테스트 절차:
 * ============================================================================
 * 1. TEST_MODE = 1 로 빌드 & 업로드
 * 2. Arduino Serial Monitor → COM9 열기 (115200) → 디버그 출력 확인
 * 3. Serial Port Utility → COM3 열기 (115200, 8N1, Hex 모드)
 * 4. Serial Port Utility에서 샘플 패킷 전송
 * 5. Serial Monitor에서 파싱 결과 확인
 * 6. Serial Port Utility에서 ACK 수신 확인 (Receive: Hex 모드로 변경)
 * 
 * ★ 샘플 패킷 (HEX):
 *   1-item:  02 08 00 01 01 00 C0 39 30 00 00 C1 03
 *   2-items: 02 0F 00 02 01 00 C0 64 00 00 00 02 00 C0 C8 00 00 00 A2 03
 * 
 * ★ Serial Port Utility Receive Setting → Hex 선택하면 ACK(02 01 00 01 03) 확인 가능
 * 
 * ============================================================================
 * 운영 모드 (TEST_MODE = 0):
 * ============================================================================
 * Serial  (USB)       → Agent 통신 (패킷 수신/ACK)
 * Serial2 (GPIO16/17) → 디버그 출력
 * WiFi + MQTT + FreeRTOS 활성화
 * 
 * 작성일: 2026-02-12
 ******************************************************************************/

//==============================================================================
// ★★★ 테스트 모드 스위치 ★★★
//==============================================================================
#define TEST_MODE  0    // 1: UART 테스트 모드,  0: 운영 모드 (WiFi+MQTT)
#define SELF_TEST  1    // 1: 자체 데이터 생성 (3초마다 UART 출력 + MQTT 전송)
                        // 0: 외부 Agent에서 UART 수신 (정상 운영)
                        // ※ TEST_MODE=0 일 때만 유효

//==============================================================================

#include <Arduino.h>

#if !TEST_MODE
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#endif

//==============================================================================
// 시리얼 포트 역할 정의
//==============================================================================

#if TEST_MODE
  #define COMM_SERIAL  Serial2    // 패킷 수신/ACK 전송 (GPIO16/17 ← Serial Port Utility)
  #define DBG_SERIAL   Serial     // 디버그 출력 (USB ← Arduino Serial Monitor)
#else
  #define COMM_SERIAL  Serial     // 패킷 수신/ACK 전송 (USB ← BC++ Agent)
  #define DBG_SERIAL   Serial2    // 디버그 출력 (GPIO16/17)
#endif

#define DBG_PRINT(x)    DBG_SERIAL.print(x)
#define DBG_PRINTLN(x)  DBG_SERIAL.println(x)
#define DBG_PRINTF(...) DBG_SERIAL.printf(__VA_ARGS__)

//==============================================================================
// 핀 및 상수 정의
//==============================================================================

#define PROTO_STX       0x02
#define PROTO_ETX       0x03
#define LED_PIN         2
#define RESET_PIN       0
#define RX_BUF_SIZE     1024

#if !TEST_MODE
#define AP_NAME         "ESP32-Wifi-Setup"
#define AP_PASSWORD     "12345678"
#define PORTAL_TIMEOUT  180
#define MQTT_QUEUE_SIZE 100
#undef MQTT_SOCKET_TIMEOUT
#define MQTT_SOCKET_TIMEOUT 2
#define SERVER_ADDRESS   "112.218.90.189"
#endif

//==============================================================================
// 공통 상태 변수
//==============================================================================

volatile unsigned long packetCount = 0;
volatile unsigned long ackSentCount = 0;
volatile unsigned long errorCount = 0;

#if TEST_MODE
unsigned long lastStatusTime = 0;
#endif

//==============================================================================
// 패킷 수신 상태 머신
//==============================================================================

enum RxState {
    RX_WAIT_STX,
    RX_GET_LEN_L,
    RX_GET_LEN_H,
    RX_GET_DATA,
    RX_GET_CHK,
    RX_GET_ETX
};

volatile RxState rxState = RX_WAIT_STX;
uint8_t rxBuffer[RX_BUF_SIZE];
int rxIndex = 0;
uint16_t rxDataLen = 0;
int rxDataCount = 0;
unsigned long rxStartTime = 0;

//==============================================================================
// ★ 운영 모드 전용 변수/객체 (setup()보다 앞에 선언!)
//==============================================================================

#if !TEST_MODE

char mqtt_server[64] = SERVER_ADDRESS;
char mqtt_port[6] = "5000";
char mqtt_client_id[32] = "ESP32_GA3_Agent";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

const char* MQTT_TOPIC_DATA   = "factory/scm_a3/data";
const char* MQTT_TOPIC_STATUS = "factory/scm_a3/status";
const char* MQTT_TOPIC_CMD    = "factory/scm_a3/command";

WiFiManager wifiManager;
Preferences preferences;

WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 64);
WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
WiFiManagerParameter custom_mqtt_client("client", "Client ID", mqtt_client_id, 32);
WiFiManagerParameter custom_mqtt_user("user", "MQTT User (optional)", mqtt_user, 32);
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Pass (optional)", mqtt_pass, 32);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

TaskHandle_t uartTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;
SemaphoreHandle_t queueMutex = NULL;

volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
volatile unsigned long mqttSentCount = 0;
volatile unsigned long mqttFailCount = 0;

unsigned long lastMqttAttempt = 0;
bool resetButtonPressed = false;
unsigned long resetButtonPressTime = 0;

struct MqttQueueItem {
    bool valid;
    uint8_t itemCount;
    uint16_t itemIds[20];
    uint8_t itemQualities[20];
    int32_t itemValues[20];
    unsigned long timestamp;
};

MqttQueueItem mqttQueue[MQTT_QUEUE_SIZE];
volatile int mqttQueueHead = 0;
volatile int mqttQueueTail = 0;

// 운영 모드 함수 프로토타입
void saveMqttConfig();
void loadMqttConfig();
void saveConfigCallback();
void resetWiFiSettings();
void checkResetButton();
void uartTask(void* param);
void selfTestTask(void* param);
void mqttTask(void* param);
void manageWiFi();
void manageMQTT();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
int getMqttQueueSize();
void processMqttQueue();
void printStatus();
void publishStatus();

#endif  // !TEST_MODE

//==============================================================================
// 유틸리티 함수
//==============================================================================

void printHexDump(const char* label, uint8_t* data, int len) 
{
    DBG_PRINTF("%s (%d bytes): ", label, len);
    for (int i = 0; i < len; i++) 
    {
        DBG_PRINTF("%02X ", data[i]);
    }
    DBG_PRINTLN();
}

const char* qualityToStr(uint8_t q)
{
    if (q >= 0xC0) return "Good";
    if (q >= 0x40) return "Uncertain";
    return "Bad";
}

//==============================================================================
// ACK 응답 (COMM_SERIAL로 전송)
//==============================================================================

void sendAckResponse()
{
    uint8_t response[5] = {
        PROTO_STX,
        0x01,       // Length Low
        0x00,       // Length High
        0x01,       // Data (ACK)
        PROTO_ETX
    };
    
    COMM_SERIAL.write(response, 5);
    COMM_SERIAL.flush();
    
    ackSentCount++;
    
    DBG_PRINTF("[ACK] Sent (#%lu): 02 01 00 01 03\n", ackSentCount);
}

//==============================================================================
// 패킷 처리
//==============================================================================

void processPacket(uint8_t* data, int len)
{
    if (len < 5) 
    {
        DBG_PRINTLN("[PARSE] ERROR: Packet too short!");
        errorCount++;
        return;
    }
    
    // 체크섬 검증
    int chkPos = len - 2;
    uint8_t calcChk = 0;
    for (int i = 1; i < chkPos; i++) 
    {
        calcChk ^= data[i];
    }
    
    if (data[chkPos] != calcChk) 
    {
        DBG_PRINTF("[PARSE] Checksum FAIL! recv=0x%02X calc=0x%02X\n", 
                   data[chkPos], calcChk);
        errorCount++;
        return;
    }
    
    DBG_PRINTLN("[PARSE] Checksum OK!");
    
    if (rxDataLen < 1) 
    {
        DBG_PRINTLN("[PARSE] Empty data (dataLen=0)");
        return;
    }
    
    // 데이터 파싱 + 출력
    int itemCount = data[3];
    DBG_PRINTF("[PARSE] Items: %d\n", itemCount);
    
    int pos = 4;
    for (int i = 0; i < itemCount && pos + 6 <= chkPos; i++) 
    {
        uint16_t id = data[pos] | (data[pos+1] << 8);
        uint8_t quality = data[pos+2];
        int32_t value = (int32_t)(
            data[pos+3] | 
            (data[pos+4] << 8) | 
            (data[pos+5] << 16) | 
            (data[pos+6] << 24)
        );
        
        DBG_PRINTF("[PARSE]   #%d: ID=%u, Quality=0x%02X(%s), Value=%d (0x%08X)\n", 
                   i, id, quality, qualityToStr(quality), value, (uint32_t)value);
        
        pos += 7;
    }
    
    int expectedEnd = 4 + (itemCount * 7);
    if (expectedEnd != chkPos) 
    {
        DBG_PRINTF("[PARSE] WARNING: %d extra bytes after items\n", chkPos - expectedEnd);
    }
    
    DBG_PRINTLN("----------------------------------------");
    
    // 운영 모드: MQTT 큐에도 추가
    #if !TEST_MODE
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) 
    {
        int nextHead = (mqttQueueHead + 1) % MQTT_QUEUE_SIZE;
        
        if (nextHead == mqttQueueTail && mqttQueue[mqttQueueTail].valid) 
        {
            DBG_PRINTLN("[QUEUE] Full! Dropping oldest.");
            mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
        }
        
        MqttQueueItem* item = &mqttQueue[mqttQueueHead];
        item->itemCount = min(itemCount, 20);
        item->timestamp = millis();
        
        int qpos = 4;
        for (int i = 0; i < item->itemCount && qpos + 6 < chkPos; i++) 
        {
            item->itemIds[i] = data[qpos] | (data[qpos+1] << 8);
            item->itemQualities[i] = data[qpos+2];
            item->itemValues[i] = (int32_t)(data[qpos+3] | (data[qpos+4] << 8) | (data[qpos+5] << 16) | (data[qpos+6] << 24));
            qpos += 7;
        }
        
        item->valid = true;
        mqttQueueHead = nextHead;
        xSemaphoreGive(queueMutex);
    }
    #endif
}

//==============================================================================
// UART 수신 처리 (COMM_SERIAL에서 읽기)
//==============================================================================

void processUART()
{
    while (COMM_SERIAL.available()) 
    {
        uint8_t b = COMM_SERIAL.read();
        
        // 수신 바이트 실시간 표시
        if (rxState == RX_WAIT_STX && b == PROTO_STX)
        {
            DBG_PRINTLN("\n========== NEW PACKET ==========");
            DBG_PRINTF("[RX] Byte[0]: 0x%02X (STX)\n", b);
        }
        else if (rxState != RX_WAIT_STX)
        {
            DBG_PRINTF("[RX] Byte[%d]: 0x%02X", rxIndex, b);
            
            switch (rxState) 
            {
                case RX_GET_LEN_L: DBG_PRINT(" (LEN_L)"); break;
                case RX_GET_LEN_H: DBG_PRINTF(" (LEN_H -> dataLen=%u)", rxDataLen | (b << 8)); break;
                case RX_GET_DATA:  DBG_PRINTF(" (DATA[%d])", rxDataCount); break;
                case RX_GET_CHK:   DBG_PRINT(" (CHK)"); break;
                case RX_GET_ETX:   DBG_PRINTF(" (%s)", b == PROTO_ETX ? "ETX OK" : "ETX FAIL"); break;
                default: break;
            }
            DBG_PRINTLN();
        }
        
        switch (rxState)
        {
            case RX_WAIT_STX:
                if (b == PROTO_STX) 
                {
                    rxIndex = 0;
                    rxBuffer[rxIndex++] = b;
                    rxStartTime = millis();
                    rxState = RX_GET_LEN_L;
                }
                else
                {
                    DBG_PRINTF("[RX] Ignored: 0x%02X (waiting for STX=0x02)\n", b);
                }
                break;
            
            case RX_GET_LEN_L:
                rxBuffer[rxIndex++] = b;
                rxDataLen = b;
                rxState = RX_GET_LEN_H;
                break;
            
            case RX_GET_LEN_H:
                rxBuffer[rxIndex++] = b;
                rxDataLen |= (b << 8);
                rxDataCount = 0;
                
                DBG_PRINTF("[RX] -> Expected data length: %u bytes\n", rxDataLen);
                
                if (rxDataLen > RX_BUF_SIZE - 12) 
                {
                    DBG_PRINTF("[RX] ERROR: Length too large! (%u > %d)\n", rxDataLen, RX_BUF_SIZE - 12);
                    errorCount++;
                    rxState = RX_WAIT_STX;
                }
                else if (rxDataLen == 0) 
                {
                    rxState = RX_GET_CHK;
                }
                else 
                {
                    rxState = RX_GET_DATA;
                }
                break;
            
            case RX_GET_DATA:
                rxBuffer[rxIndex++] = b;
                rxDataCount++;
                if (rxDataCount >= rxDataLen) 
                {
                    DBG_PRINTF("[RX] -> All %u data bytes received\n", rxDataLen);
                    rxState = RX_GET_CHK;
                }
                break;
            
            case RX_GET_CHK:
                rxBuffer[rxIndex++] = b;
                rxState = RX_GET_ETX;
                break;
            
            case RX_GET_ETX:
                rxBuffer[rxIndex++] = b;
                
                if (b == PROTO_ETX)
                {
                    printHexDump("[RX] Full Packet", rxBuffer, rxIndex);
                    DBG_PRINTF("[RX] Packet OK! totalLen=%d, dataLen=%u\n", rxIndex, rxDataLen);
                    
                    sendAckResponse();
                    processPacket(rxBuffer, rxIndex);
                    
                    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                    packetCount++;
                }
                else 
                {
                    DBG_PRINTF("[RX] ERROR: Expected ETX(0x03), got 0x%02X\n", b);
                    printHexDump("[RX] Bad Packet", rxBuffer, rxIndex);
                    errorCount++;
                }
                
                rxState = RX_WAIT_STX;
                break;
        }
        
        // 타임아웃 체크 (1초)
        if (rxState != RX_WAIT_STX && millis() - rxStartTime > 1000) 
        {
            DBG_PRINTLN("[RX] TIMEOUT! Resetting state machine.");
            if (rxIndex > 0) 
            {
                printHexDump("[RX] Incomplete data", rxBuffer, rxIndex);
            }
            errorCount++;
            rxState = RX_WAIT_STX;
        }
        
        // 버퍼 오버플로우 체크
        if (rxIndex >= RX_BUF_SIZE - 2) 
        {
            DBG_PRINTLN("[RX] BUFFER OVERFLOW!");
            errorCount++;
            rxState = RX_WAIT_STX;
        }
    }
}

//==============================================================================
// 테스트 모드 상태 출력
//==============================================================================

#if TEST_MODE
void printTestStatus()
{
    DBG_PRINTLN("\n============ TEST STATUS ============");
    DBG_PRINTF("Uptime:     %lu sec\n", millis() / 1000);
    DBG_PRINTF("Free Heap:  %d bytes\n", ESP.getFreeHeap());
    DBG_PRINTLN("-------------------------------------");
    DBG_PRINTF("Packets OK: %lu\n", packetCount);
    DBG_PRINTF("ACK Sent:   %lu\n", ackSentCount);
    DBG_PRINTF("Errors:     %lu\n", errorCount);
    DBG_PRINTF("RX State:   %d (%s)\n", rxState, 
               rxState == RX_WAIT_STX ? "Waiting STX" : "In progress");
    DBG_PRINTLN("=====================================");
    DBG_PRINTLN("Send HEX via Serial Port Utility (COM3 -> GPIO16)");
    DBG_PRINTLN("  1-item: 02 08 00 01 01 00 C0 39 30 00 00 C1 03");
    DBG_PRINTLN("  2-item: 02 0F 00 02 01 00 C0 64 00 00 00 02 00 C0 C8 00 00 00 A2 03");
    DBG_PRINTLN("=====================================\n");
}
#endif

//==============================================================================
// Setup
//==============================================================================

void setup()
{
    // GPIO 초기화
    pinMode(LED_PIN, OUTPUT);
    pinMode(RESET_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN, LOW);
    
#if TEST_MODE
    //==========================================================================
    // 테스트 모드 초기화
    //==========================================================================
    Serial.begin(115200);                          // USB (COM9) → 디버그 출력
    delay(100);
    Serial2.begin(115200, SERIAL_8N1, 16, 17);     // GPIO16/17 (COM3) → 패킷 수신
    delay(100);
    
    delay(1000);
    
    DBG_PRINTLN("\n\n");
    DBG_PRINTLN("####################################################");
    DBG_PRINTLN("#                                                  #");
    DBG_PRINTLN("#   ESP32 UART Reception TEST MODE (v4-test-v2)    #");
    DBG_PRINTLN("#                                                  #");
    DBG_PRINTLN("#   WiFi/MQTT: DISABLED                            #");
    DBG_PRINTLN("#                                                  #");
    DBG_PRINTLN("#   Serial (USB/COM9)  = Debug output              #");
    DBG_PRINTLN("#   Serial2 (GPIO16/17) = Packet RX/TX (COM3)      #");
    DBG_PRINTLN("#                                                  #");
    DBG_PRINTLN("####################################################");
    DBG_PRINTLN();
    DBG_PRINTLN("Wiring:");
    DBG_PRINTLN("  USB-UART Adapter TX  -->  ESP32 GPIO16 (RX2)");
    DBG_PRINTLN("  USB-UART Adapter RX  <--  ESP32 GPIO17 (TX2)");
    DBG_PRINTLN("  USB-UART Adapter GND ---  ESP32 GND");
    DBG_PRINTLN();
    DBG_PRINTLN("Protocol: [STX(02)] [LEN_L] [LEN_H] [DATA...] [CHK] [ETX(03)]");
    DBG_PRINTLN("DATA:     [ItemCount(1)] [ID_L ID_H Quality Val0 Val1 Val2 Val3] x N");
    DBG_PRINTLN("CHK:      XOR of all bytes from LEN_L to last DATA byte");
    DBG_PRINTLN();
    DBG_PRINTLN("========== Sample Packets (HEX) ==========");
    DBG_PRINTLN();
    DBG_PRINTLN("1) 1-item (ID=1, Q=Good, Value=12345):");
    DBG_PRINTLN("   02 08 00 01 01 00 C0 39 30 00 00 C1 03");
    DBG_PRINTLN();
    DBG_PRINTLN("2) 2-items (ID=1,V=100 / ID=2,V=200):");
    DBG_PRINTLN("   02 0F 00 02 01 00 C0 64 00 00 00 02 00 C0 C8 00 00 00 A2 03");
    DBG_PRINTLN();
    DBG_PRINTLN("3) 3-items (ID=10,V=500 / ID=11,Q=Bad,V=0 / ID=12,V=-1):");
    DBG_PRINTLN("   02 16 00 03 0A 00 C0 F4 01 00 00 0B 00 00 00 00 00 00 0C 00 C0 FF FF FF FF 98 03");
    DBG_PRINTLN();
    DBG_PRINTLN("4) Error tests:");
    DBG_PRINTLN("   Bad ETX:      02 08 00 01 01 00 C0 39 30 00 00 C1 FF");
    DBG_PRINTLN("   Bad Checksum: 02 08 00 01 01 00 C0 39 30 00 00 AA 03");
    DBG_PRINTLN();
    DBG_PRINTLN("===========================================");
    DBG_PRINTLN("Waiting for packets on Serial2 (GPIO16)...\n");

#else
    //==========================================================================
    // 운영 모드 초기화
    //==========================================================================
    Serial.begin(115200);                          // USB → Agent 통신
    delay(100);
    Serial2.begin(115200, SERIAL_8N1, 16, 17);     // GPIO16/17 → 디버그
    delay(100);
    
    queueMutex = xSemaphoreCreateMutex();
    
    for (int i = 0; i < MQTT_QUEUE_SIZE; i++) 
    {
        mqttQueue[i].valid = false;
    }
    
    delay(500);
    
    DBG_PRINTLN("\n================================================");
    DBG_PRINTLN("  ESP32 UART + MQTT Gateway (v4)");
    DBG_PRINTLN("  - FreeRTOS Task Separation");
    DBG_PRINTLN("  - ACK Priority Response");
    DBG_PRINTLN("================================================\n");
    
    loadMqttConfig();
    
    wifiManager.setDebugOutput(true);
    wifiManager.setConfigPortalTimeout(PORTAL_TIMEOUT);
    wifiManager.setConnectTimeout(20);
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_client);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    
    wifiManager.setAPCallback([](WiFiManager* wm) {
        DBG_PRINTLN("\n[WiFiManager] Entered config portal");
        DBG_PRINTF("  AP: %s / %s\n", AP_NAME, AP_PASSWORD);
        DBG_PRINTLN("  Connect and go to 192.168.4.1");
        digitalWrite(LED_PIN, HIGH);
    });
    
    DBG_PRINTLN("[WiFiManager] Attempting auto-connect...");
    
    if (!wifiManager.autoConnect(AP_NAME, AP_PASSWORD)) 
    {
        DBG_PRINTLN("[WiFiManager] Failed! Restarting...");
        delay(3000);
        ESP.restart();
    }
    
    wifiConnected = true;
    digitalWrite(LED_PIN, LOW);
    
    DBG_PRINTLN("\n[WiFi] Connected!");
    DBG_PRINTF("  IP: %s\n", WiFi.localIP().toString().c_str());
    
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(RX_BUF_SIZE);
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT);
    
    xTaskCreatePinnedToCore(mqttTask, "MQTT_Task", 8192, NULL, 1, &mqttTaskHandle, 0);
    
    #if SELF_TEST
    // ★ 자체 테스트: UART 수신 대신 자체 데이터 생성 태스크
    xTaskCreatePinnedToCore(selfTestTask, "SelfTest_Task", 4096, NULL, 2, &uartTaskHandle, 1);
    DBG_PRINTLN("[RTOS] Tasks created! (SELF_TEST mode - no external UART input)");
    DBG_PRINTLN("[SELF_TEST] Will generate sample data every 3 seconds");
    #else
    xTaskCreatePinnedToCore(uartTask, "UART_Task", 4096, NULL, 3, &uartTaskHandle, 1);
    DBG_PRINTLN("[RTOS] Tasks created!");
    #endif
#endif
}

//==============================================================================
// Main Loop
//==============================================================================

void loop()
{
#if TEST_MODE
    processUART();
    
    if (millis() - lastStatusTime > 10000)
    {
        lastStatusTime = millis();
        printTestStatus();
    }
    
#else
    checkResetButton();
    
    static unsigned long lastSt = 0;
    if (millis() - lastSt > 30000) 
    {
        lastSt = millis();
        printStatus();
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
#endif
}


//##############################################################################
//
//  이하 운영 모드 전용 함수 (TEST_MODE=0 일 때만 컴파일)
//
//##############################################################################

#if !TEST_MODE

//==============================================================================
// NVS 설정 저장/로드
//==============================================================================

void saveMqttConfig() 
{
    preferences.begin("mqtt", false);
    preferences.putString("server", mqtt_server);
    preferences.putString("port", mqtt_port);
    preferences.putString("client", mqtt_client_id);
    preferences.putString("user", mqtt_user);
    preferences.putString("pass", mqtt_pass);
    preferences.end();
}

void loadMqttConfig() 
{
    preferences.begin("mqtt", true);
    preferences.getString("server", SERVER_ADDRESS).toCharArray(mqtt_server, 64);
    preferences.getString("port", "5000").toCharArray(mqtt_port, 6);
    preferences.getString("client", "ESP32_GA3_Agent").toCharArray(mqtt_client_id, 32);
    preferences.getString("user", "").toCharArray(mqtt_user, 32);
    preferences.getString("pass", "").toCharArray(mqtt_pass, 32);
    preferences.end();
    DBG_PRINTF("[NVS] Server: %s:%s, Client: %s\n", mqtt_server, mqtt_port, mqtt_client_id);
}

void saveConfigCallback() 
{
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_client_id, custom_mqtt_client.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    saveMqttConfig();
}

//==============================================================================
// WiFi 설정 초기화 / 리셋 버튼
//==============================================================================

void resetWiFiSettings() 
{
    wifiManager.resetSettings();
    preferences.begin("mqtt", false);
    preferences.clear();
    preferences.end();
    delay(1000);
    ESP.restart();
}

void checkResetButton() 
{
    if (digitalRead(RESET_PIN) == LOW) 
    {
        if (!resetButtonPressed) { resetButtonPressed = true; resetButtonPressTime = millis(); } 
        else if (millis() - resetButtonPressTime > 5000) 
        {
            for (int i = 0; i < 10; i++) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
            resetWiFiSettings();
        }
    } 
    else { resetButtonPressed = false; }
}

//==============================================================================
// FreeRTOS 태스크
//==============================================================================

void uartTask(void* param) 
{
    DBG_PRINTLN("[UART Task] Started on Core 1");
    while (true) { processUART(); vTaskDelay(1); }
}

//==============================================================================
// Self Test 태스크 - 3초마다 샘플 데이터 생성 → UART 출력 + MQTT 전송
//==============================================================================

void selfTestTask(void* param)
{
    DBG_PRINTLN("[SelfTest Task] Started on Core 1");
    DBG_PRINTLN("[SelfTest] Generating sample packets every 3 seconds...\n");
    
    uint32_t seqNo = 0;
    
    while (true)
    {
        seqNo++;
        
        //----------------------------------------------------------------------
        // 샘플 데이터 생성 (3개 아이템, 값은 매번 변경)
        //----------------------------------------------------------------------
        const int ITEM_COUNT = 3;
        
        struct {
            uint16_t id;
            uint8_t  quality;
            int32_t  value;
        } items[ITEM_COUNT] = {
            { 1, 0xC0, (int32_t)(1000 + (seqNo * 10) % 9000) },       // ID=1: 1000~9990 변동
            { 2, 0xC0, (int32_t)(random(0, 500)) },                     // ID=2: 0~499 랜덤
            { 3, (uint8_t)(seqNo % 5 == 0 ? 0x00 : 0xC0),              // ID=3: 5회마다 Bad Quality
                 (int32_t)(analogRead(34)) }                             // ID=3: ADC 값 (GPIO34)
        };
        
        //----------------------------------------------------------------------
        // 패킷 조립: [STX] [LEN_L] [LEN_H] [ItemCount] [Items...] [CHK] [ETX]
        //----------------------------------------------------------------------
        uint8_t pkt[128];
        int idx = 0;
        
        // STX
        pkt[idx++] = PROTO_STX;
        
        // Data: ItemCount(1) + Items(7 * N)
        uint16_t dataLen = 1 + (ITEM_COUNT * 7);
        pkt[idx++] = dataLen & 0xFF;        // LEN_L
        pkt[idx++] = (dataLen >> 8) & 0xFF;  // LEN_H
        
        // ItemCount
        pkt[idx++] = ITEM_COUNT;
        
        // Items
        for (int i = 0; i < ITEM_COUNT; i++)
        {
            pkt[idx++] = items[i].id & 0xFF;
            pkt[idx++] = (items[i].id >> 8) & 0xFF;
            pkt[idx++] = items[i].quality;
            pkt[idx++] = (items[i].value)       & 0xFF;
            pkt[idx++] = (items[i].value >> 8)   & 0xFF;
            pkt[idx++] = (items[i].value >> 16)  & 0xFF;
            pkt[idx++] = (items[i].value >> 24)  & 0xFF;
        }
        
        // Checksum (XOR: LEN_L ~ 마지막 DATA)
        uint8_t chk = 0;
        for (int i = 1; i < idx; i++)
        {
            chk ^= pkt[i];
        }
        pkt[idx++] = chk;
        
        // ETX
        pkt[idx++] = PROTO_ETX;
        
        //----------------------------------------------------------------------
        // UART(USB)로 패킷 출력
        //----------------------------------------------------------------------
        COMM_SERIAL.write(pkt, idx);
        COMM_SERIAL.flush();
        
        //----------------------------------------------------------------------
        // 디버그 출력
        //----------------------------------------------------------------------
        DBG_PRINTF("\n[SELF_TEST #%lu] Packet sent (%d bytes): ", seqNo, idx);
        for (int i = 0; i < idx; i++) { DBG_PRINTF("%02X ", pkt[i]); }
        DBG_PRINTLN();
        
        for (int i = 0; i < ITEM_COUNT; i++)
        {
            DBG_PRINTF("  Item #%d: ID=%u, Quality=0x%02X(%s), Value=%d\n",
                       i, items[i].id, items[i].quality,
                       items[i].quality >= 0xC0 ? "Good" : "Bad",
                       items[i].value);
        }
        
        //----------------------------------------------------------------------
        // MQTT 큐에 추가
        //----------------------------------------------------------------------
        if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            int nextHead = (mqttQueueHead + 1) % MQTT_QUEUE_SIZE;
            
            if (nextHead == mqttQueueTail && mqttQueue[mqttQueueTail].valid)
            {
                mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
            }
            
            MqttQueueItem* qItem = &mqttQueue[mqttQueueHead];
            qItem->itemCount = ITEM_COUNT;
            qItem->timestamp = millis();
            
            for (int i = 0; i < ITEM_COUNT; i++)
            {
                qItem->itemIds[i] = items[i].id;
                qItem->itemQualities[i] = items[i].quality;
                qItem->itemValues[i] = items[i].value;
            }
            
            qItem->valid = true;
            mqttQueueHead = nextHead;
            packetCount++;
            
            xSemaphoreGive(queueMutex);
            
            DBG_PRINTF("[SELF_TEST] Queued for MQTT (total packets: %lu)\n", packetCount);
        }
        
        // 3초 대기
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

void mqttTask(void* param) 
{
    DBG_PRINTLN("[MQTT Task] Started on Core 0");
    while (true) 
    {
        manageWiFi(); manageMQTT(); processMqttQueue();
        static unsigned long lp = 0;
        if (millis() - lp > 30000) { lp = millis(); publishStatus(); }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

//==============================================================================
// WiFi / MQTT 관리
//==============================================================================

void manageWiFi()
{
    static unsigned long dt = 0;
    if (WiFi.status() == WL_CONNECTED) 
    {
        if (!wifiConnected) { wifiConnected = true; dt = 0; DBG_PRINTF("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str()); }
    } 
    else 
    {
        if (wifiConnected) { wifiConnected = false; mqttConnected = false; dt = millis(); DBG_PRINTLN("[WiFi] Disconnected!"); }
        if (dt > 0 && millis() - dt > 120000) { ESP.restart(); }
    }
}

void manageMQTT()
{
    if (!wifiConnected) return;
    if (mqttClient.connected()) 
    {
        if (!mqttConnected) { mqttConnected = true; DBG_PRINTLN("[MQTT] Connected!"); }
        mqttClient.loop();
    } 
    else 
    {
        if (mqttConnected) { mqttConnected = false; DBG_PRINTLN("[MQTT] Disconnected!"); }
        if (millis() - lastMqttAttempt > 5000) { lastMqttAttempt = millis(); connectMQTT(); }
    }
}

void connectMQTT()
{
    DBG_PRINTF("[MQTT] Connecting to %s:%s... ", mqtt_server, mqtt_port);
    bool c = (strlen(mqtt_user) > 0) ? mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass) : mqttClient.connect(mqtt_client_id);
    if (c) { DBG_PRINTLN("OK!"); mqttConnected = true; mqttClient.subscribe(MQTT_TOPIC_CMD); publishStatus(); } 
    else { DBG_PRINTF("FAILED (rc=%d)\n", mqttClient.state()); }
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    char msg[256]; memcpy(msg, payload, min((int)length, 255)); msg[min((int)length, 255)] = '\0';
    if (strcmp(topic, MQTT_TOPIC_CMD) == 0) 
    {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, msg) == DeserializationError::Ok) 
        {
            const char* cmd = doc["cmd"];
            if (strcmp(cmd, "status") == 0) publishStatus();
            else if (strcmp(cmd, "reset_count") == 0) { packetCount = mqttSentCount = mqttFailCount = ackSentCount = 0; }
            else if (strcmp(cmd, "led_on") == 0) digitalWrite(LED_PIN, HIGH);
            else if (strcmp(cmd, "led_off") == 0) digitalWrite(LED_PIN, LOW);
            else if (strcmp(cmd, "wifi_reset") == 0) resetWiFiSettings();
            else if (strcmp(cmd, "reboot") == 0) { delay(500); ESP.restart(); }
        }
    }
}

//==============================================================================
// MQTT 큐 처리
//==============================================================================

int getMqttQueueSize()
{
    int s = 0;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) 
    {
        for (int i = 0; i < MQTT_QUEUE_SIZE; i++) { if (mqttQueue[i].valid) s++; }
        xSemaphoreGive(queueMutex);
    }
    return s;
}

void processMqttQueue()
{
    if (!mqttConnected) return;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) 
    {
        if (!mqttQueue[mqttQueueTail].valid) { xSemaphoreGive(queueMutex); return; }
        MqttQueueItem item = mqttQueue[mqttQueueTail];
        mqttQueue[mqttQueueTail].valid = false;
        mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
        xSemaphoreGive(queueMutex);
        
        StaticJsonDocument<512> doc;
        doc["device"] = mqtt_client_id; doc["timestamp"] = item.timestamp; doc["packet_id"] = packetCount;
        for (int i = 0; i < item.itemCount; i++) 
        {
            doc["v_" + String(item.itemIds[i])] = item.itemValues[i];
            doc["q_" + String(item.itemIds[i])] = item.itemQualities[i];
        }
        char buf[512]; serializeJson(doc, buf);
        if (mqttClient.publish(MQTT_TOPIC_DATA, buf)) mqttSentCount++; else mqttFailCount++;
    }
}

//==============================================================================
// 상태 출력/발행
//==============================================================================

void printStatus()
{
    DBG_PRINTLN("\n============ STATUS ============");
    DBG_PRINTF("Uptime: %lu sec, Heap: %d\n", millis() / 1000, ESP.getFreeHeap());
    DBG_PRINTF("Packets: %lu, ACK: %lu\n", packetCount, ackSentCount);
    DBG_PRINTF("WiFi: %s, MQTT: %s\n", wifiConnected ? "OK" : "NO", mqttConnected ? "OK" : "NO");
    DBG_PRINTF("MQTT Sent: %lu, Fail: %lu, Queue: %d\n", mqttSentCount, mqttFailCount, getMqttQueueSize());
    DBG_PRINTLN("================================\n");
}

void publishStatus()
{
    if (!mqttConnected) return;
    StaticJsonDocument<384> doc;
    doc["device"] = mqtt_client_id; doc["uptime"] = millis() / 1000;
    doc["packets"] = packetCount; doc["ack_sent"] = ackSentCount;
    doc["mqtt_sent"] = mqttSentCount; doc["mqtt_fail"] = mqttFailCount;
    doc["queue_size"] = getMqttQueueSize(); doc["rssi"] = WiFi.RSSI();
    doc["heap"] = ESP.getFreeHeap(); doc["ip"] = WiFi.localIP().toString();
    doc["version"] = "v4";
    char buf[384]; serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

#endif  // !TEST_MODE