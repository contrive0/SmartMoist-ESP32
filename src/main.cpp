#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h> // 비휘발성 메모리(NVS) 라이브러리 추가

// OLED 설정 (128x64 해상도)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 센서 객체
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;

// Preferences 객체 선언
Preferences preferences;

// ESP32-C3 핀 설정
#define RELAY_PIN 5           // 가습기 릴레이(또는 테스트용 LED) 제어 핀
#define SDA_PIN 8             // I2C SDA
#define SCL_PIN 9             // I2C SCL

// 택트 버튼 핀 설정 (INPUT_PULLUP 사용: 누르면 LOW 입력)
#define BTN_SELECT 4          // 선택(에디터) 버튼
#define BTN_UP     6          // 값 증가 버튼
#define BTN_DOWN   7          // 값 감소 버튼

// 제어용 습도 변수 (기본값 설정)
float onHumidity = 50.0;      // 이 습도 이하로 떨어지면 가습기 ON
float offHumidity = 60.0;     // 이 습도 이상으로 올라가면 가습기 OFF

// 설정 모드 상태 (0: 기본 화면, 1: OFF 습도 설정 중, 2: ON 습도 설정 중)
int editMode = 0; 

// 버튼 디바운싱 및 3초 꾹 누름 감지를 위한 변수
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200; // 200ms 딜레이

unsigned long selectPressTime = 0;       // 선택 버튼 누르기 시작한 시간
bool selectLongPressHandled = false;     // 3초 장꾹 누름 중복 실행 방지 플래그

void handleButtons() {
  unsigned long currentTime = millis();

  // 1. 선택(에디터) 버튼 3초 꾹 누르기 감지 로직
  int selectState = digitalRead(BTN_SELECT);
  if (selectState == LOW) { // 버튼을 누르고 있는 동안
    if (selectPressTime == 0) {
      selectPressTime = currentTime;     // 누르기 시작한 시간 기록
      selectLongPressHandled = false;
    } else if (!selectLongPressHandled && (currentTime - selectPressTime >= 3000)) {
      // 3초(3000ms) 이상 꾹 누르고 있을 때 동작 실행
      editMode = (editMode + 1) % 3;     // 0 -> 1 -> 2 -> 0 순환
      selectLongPressHandled = true;     // 중복 실행 방지

      // 설정 모드가 끝나고 다시 기본 화면(0)으로 돌아갈 때 메모리에 저장
      if (editMode == 0) {
        preferences.begin("humidifier", false); // "humidifier" 네임스페이스로 쓰기 모드 오픈
        preferences.putFloat("onHumi", onHumidity);
        preferences.putFloat("offHumi", offHumidity);
        preferences.end();
        Serial.println("설정값이 플래시 메모리에 안전하게 저장되었습니다!");
      }
    }
  } else { // 버튼에서 손을 뗄 때 초기화
    selectPressTime = 0;
    selectLongPressHandled = false;
  }

  // 2. 일반 버튼 디바운스 체크 (UP, DOWN 버튼용)
  if (currentTime - lastButtonPress < debounceDelay) return;

  // 위 버튼 (UP) - 값 증가
  if (digitalRead(BTN_UP) == LOW) {
    lastButtonPress = currentTime;
    if (editMode == 1) { // OFF 습도 수정
      offHumidity += 1.0;
      if (offHumidity > 99.0) offHumidity = 99.0;
    } else if (editMode == 2) { // ON 습도 수정
      onHumidity += 1.0;
      if (onHumidity >= offHumidity) onHumidity = offHumidity - 1.0; // OFF보다 클 수 없음
    }
  }

  // 아래 버튼 (DOWN) - 값 감소
  if (digitalRead(BTN_DOWN) == LOW) {
    lastButtonPress = currentTime;
    if (editMode == 1) { // OFF 습도 수정
      offHumidity -= 1.0;
      if (offHumidity <= onHumidity) offHumidity = onHumidity + 1.0; // ON보다 작을 수 없음
    } else if (editMode == 2) { // ON 습도 수정
      onHumidity -= 1.0;
      if (onHumidity < 0.0) onHumidity = 0.0;
    }
  }
}//handleButtons

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // 부팅 시 NVS 메모리에서 저장된 설정값 불러오기
  preferences.begin("humidifier", true); // 읽기 전용으로 열기
  onHumidity = preferences.getFloat("onHumi", 50.0);   // 저장된 값이 없으면 기본값 50.0 사용
  offHumidity = preferences.getFloat("offHumi", 60.0); // 저장된 값이 없으면 기본값 60.0 사용
  preferences.end();

  // ESP32-C3 I2C 핀 지정
  Wire.begin(SDA_PIN, SCL_PIN); 

  // 제어 핀 설정
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 초기 상태 LOW

  // 버튼 핀 설정 (내부 풀업 저항 활성화)
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // 1. OLED 초기화 (I2C 주소 0x3C)
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED 연결 실패!"));
    while (1) { delay(100); }
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // 2. AHT20 센서 초기화
  if (!aht.begin()) {
    Serial.println("AHT20 센서 연결 실패!");
  }

  // 3. BMP280 센서 초기화
  if (!bmp.begin(0x76)) { 
    if (!bmp.begin(0x77)) {
      Serial.println("BMP280 센서 연결 실패!");
    }
  }
}//setup

void loop() {
  // 버튼 입력 처리
  handleButtons();

  // AHT20 센서 측정값 가져오기
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);

  float currentTemp = temp.temperature;
  float currentHumidity = humidity.relative_humidity;

  // 가습기 제어 로직 (히스테리시스)
  static bool humidifierState = false;
  if (currentHumidity >= offHumidity) {
    humidifierState = false; // OFF
  } else if (currentHumidity <= onHumidity) {
    humidifierState = true;  // ON
  }
  digitalWrite(RELAY_PIN, humidifierState ? HIGH : LOW);

  // ================= OLED 화면 구성 =================
  display.clearDisplay();

  // 1. 상단 타이틀 (크기 1)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(3, 0);//왼쪽에서 2띄우기.
  if (editMode == 0) {
    display.print("# Smart Humidifier #"); 
  } else if (editMode == 1) {
    display.print("[SET MODE] Change OFF ");
  } else if (editMode == 2) {
    display.print("[SET MODE] Change ON  ");
  }

  // 2. 현재 온/습도 (크기 2 - 한 줄에 크게 표시하며 가운데 정렬)
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(21, 16); //가운데로  X,Y
  
  display.print((int)currentTemp);
  display.print("C ");
  display.print((int)currentHumidity);
  display.print("%");

  // 3. ON / OFF 설정 습도 표시 (반전 기능 적용, 크기 1)
  display.setTextSize(1);
  display.setCursor(0, 38);
  display.setTextColor(SSD1306_WHITE);
  display.print("Set: ");

  // --- ON 습도 출력 부분 (editMode == 2 일 때 반전) ---
  if (editMode == 2) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); 
  } else {
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); 
  }
  display.print("ON:");
  display.print((int)onHumidity);
  display.print("%");

  // 구분 기호 (일반 표시)
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.print(" / ");

  // --- OFF 습도 출력 부분 (editMode == 1 일 때 반전) ---
  if (editMode == 1) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); 
  } else {
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); 
  }
  display.print("OFF:");
  display.print((int)offHumidity);
  display.print("%");

  // 색상 기본값(흰색) 복원
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);

  // 4. 하단 동작 상태 표시 (크기 1)
  display.setCursor(0, 54);
  display.print("Relay: ");
  display.print(humidifierState ? "[ON]" : "[OFF]");
  display.print(editMode == 0 ? " (Running)" : " (Editing)");

  display.display(); // 화면 최종 갱신

  // 시리얼 모니터 출력
  Serial.print("온도:"); Serial.print(currentTemp, 1);
  Serial.print("C | 습도:"); Serial.print(currentHumidity, 1);
  Serial.print("% | ON설정:"); Serial.print(onHumidity, 0);
  Serial.print("% | OFF설정:"); Serial.print(offHumidity, 0);
  Serial.println(humidifierState ? "% | 릴레이:ON" : "% | 릴레이:OFF");

  delay(100); // 디바운스 및 루프 주기 조정
}//loop
