#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <Adafruit_NeoPixel.h>
#include "driver/mcpwm_prelude.h"   // ← MCPWM en lugar de ledc.h

// --- PINES ---
#define SDA_PIN      2
#define SCL_PIN      1
#define SERVO_PIN   13
#define BTN1_PIN     4
#define BTN2_PIN     5
#define PIN_RGB     48

// --- OLED Y RGB ---
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Adafruit_NeoPixel pixel(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// --- TECLADO ---
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'}, {'4','5','6','B'}, {'7','8','9','C'}, {'*','0','#','D'}
};
byte rowPins[ROWS] = {6, 7, 15, 16};
byte colPins[COLS] = {17, 18, 8, 9};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- MCPWM ---
// 819200 Hz × 16384 ticks = 50 Hz exactos (equivalente a 14-bit)
// compareValue = 408 + (ángulo × 1740) / 180
#define PWM_PERIOD   16384
const uint32_t CMP_MIN = 408;
const uint32_t CMP_MAX = 2148;

static mcpwm_cmpr_handle_t servoComparator = NULL;

// --- ESTADOS ---
enum Estado {
  EST_LOGIN, EST_BLOQUEADO, EST_BIENVENIDA, EST_ERROR_PASS,
  EST_MENU, EST_MODO_B_INGRESO, EST_MODO_B_EJECUTANDO, EST_MODO_B_ERROR, EST_MODO_B_RETORNO
};
Estado estadoActual = EST_LOGIN;

// --- VARIABLES ---
const char* passwords[] = {"1204", "2206", "1011"};
const char* nombres[]   = {"RUBEN", "JUAN", "MANU"};
char passIngresada[5]   = "****";
int posCursor = 0;
int intentos  = 0;

char anguloStr[4] = "000";
int posAng = 0;
uint32_t anguloObj = 0;
uint32_t anguloAct = 0;
uint32_t durezaX100  = 0;   // duty × 100 para mostrar 2 decimales
uint32_t duracionMs  = 0;
uint32_t anguloRetorno = 0;
bool btn1LastState = HIGH;

volatile uint32_t ticksTemp = 0;
volatile uint32_t ticksMov  = 0;
volatile uint32_t ticksBtn2 = 0;

hw_timer_t *timerHw = NULL;

void IRAM_ATTR onTimer() {
  if (ticksTemp > 0) ticksTemp--;
  if (ticksMov  > 0) ticksMov++;
  if (digitalRead(BTN2_PIN) == LOW) ticksBtn2++;
  else if (ticksBtn2 < 400)         ticksBtn2 = 0;
}

// --- FUNCIONES ---
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}
void initServo() {
  mcpwm_timer_handle_t timer = NULL;

  mcpwm_timer_config_t timer_cfg;
  memset(&timer_cfg, 0, sizeof(timer_cfg));
  timer_cfg.group_id      = 0;
  timer_cfg.clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT;
  timer_cfg.resolution_hz = 819200;          // 50 Hz × 16384 ticks
  timer_cfg.count_mode    = MCPWM_TIMER_COUNT_MODE_UP;
  timer_cfg.period_ticks  = PWM_PERIOD;
  mcpwm_new_timer(&timer_cfg, &timer);

  mcpwm_oper_handle_t oper = NULL;
  mcpwm_operator_config_t oper_cfg;
  memset(&oper_cfg, 0, sizeof(oper_cfg));
  oper_cfg.group_id = 0;
  mcpwm_new_operator(&oper_cfg, &oper);
  mcpwm_operator_connect_timer(oper, timer);

  mcpwm_comparator_config_t cmp_cfg;
  memset(&cmp_cfg, 0, sizeof(cmp_cfg));
  cmp_cfg.flags.update_cmp_on_tez = true;
  mcpwm_new_comparator(oper, &cmp_cfg, &servoComparator);

  mcpwm_gen_handle_t gen = NULL;
  mcpwm_generator_config_t gen_cfg;
  memset(&gen_cfg, 0, sizeof(gen_cfg));
  gen_cfg.gen_gpio_num = SERVO_PIN;
  mcpwm_new_generator(oper, &gen_cfg, &gen);

  mcpwm_comparator_set_compare_value(servoComparator, CMP_MIN);

  mcpwm_generator_set_action_on_timer_event(gen,
    MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                  MCPWM_TIMER_EVENT_EMPTY,
                                  MCPWM_GEN_ACTION_HIGH));
  mcpwm_generator_set_action_on_compare_event(gen,
    MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                    servoComparator,
                                    MCPWM_GEN_ACTION_LOW));

  mcpwm_timer_enable(timer);
  mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
}
void moverServo(uint32_t ang) {
  uint32_t cmp = CMP_MIN + (ang * (CMP_MAX - CMP_MIN)) / 180;
  mcpwm_comparator_set_compare_value(servoComparator, cmp);
}

void resetLogin() {
  strcpy(passIngresada, "****");
  posCursor = 0;
  estadoActual = EST_LOGIN;
  setRGB(0, 0, 255);
}

void actualizarPantalla(const char* t1, const char* t2) {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setCursor(0, 0);  oled.println(t1);
  oled.setTextSize(2); oled.setCursor(30, 30); oled.println(t2);
  oled.display();
}

void mostrarModoB(bool error) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0,  0); oled.println("MODO PROGRAMADO");
  if (error) {
    oled.setCursor(0, 16); oled.println("ANGULO: ERROR");
    oled.setCursor(0, 32); oled.println("DUREZA: - - -");
    oled.setCursor(0, 48); oled.println("DURACION: - - -");
  } else {
    char buf[22];
    oled.setCursor(0, 16);
    sprintf(buf, "ANGULO: %d", (int)anguloObj);   oled.println(buf);
    oled.setCursor(0, 32);
    sprintf(buf, "DUREZA: %u.%02u%%", (unsigned)(durezaX100/100), (unsigned)(durezaX100%100));
    oled.println(buf);
    oled.setCursor(0, 48);
    sprintf(buf, "DURACION: %dms", (int)duracionMs); oled.println(buf);
  }
  oled.display();
}

// Menú en 2 líneas para que quepa en la OLED (textSize 2 desbordaba)
void mostrarMenu() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0,  0); oled.println("== MENU PRINCIPAL ==");
  oled.setCursor(0, 20); oled.println("A: MANUAL");
  oled.setCursor(0, 35); oled.println("B: PROGRAMADO");
  oled.display();
}

void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oled.setTextColor(WHITE);
  pixel.begin();
  initServo();
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN1_PIN, INPUT_PULLUP);

  timerHw = timerBegin(1000000);
  timerAttachInterrupt(timerHw, &onTimer);
  timerAlarm(timerHw, 10000, true, 0); // Tick cada 10 ms

  resetLogin();
}

void loop() {
  char key = keypad.getKey();
  bool btn1State   = digitalRead(BTN1_PIN);
  bool btn1Pressed = (btn1State == LOW && btn1LastState == HIGH);
  btn1LastState = btn1State;

  if (estadoActual != EST_LOGIN && ticksBtn2 >= 400) {
    ticksBtn2 = 0;
    moverServo(0);
    resetLogin();
    return;
  }

  switch (estadoActual) {

    case EST_LOGIN:
      actualizarPantalla("INGRESE CLAVE", passIngresada);
      if (key) {
        if (key >= '0' && key <= '9') {
          passIngresada[posCursor] = key;
        } else if (key == '*') {
          if (posCursor < 3) posCursor++;
        } else if (key == 'D') {
          resetLogin();
        } else if (key == '#') {
          if (strchr(passIngresada, '*') == NULL) {
            int userIdx = -1;
            for (int i = 0; i < 3; i++)          // ← corregido: era i<2
              if (strcmp(passIngresada, passwords[i]) == 0) userIdx = i;

            if (userIdx != -1) {
              setRGB(0, 255, 0);
              actualizarPantalla("BIENVENIDO", nombres[userIdx]);
              ticksTemp = 500;
              estadoActual = EST_BIENVENIDA;
            } else {
              intentos++;
              setRGB(255, 255, 0);
              actualizarPantalla("CLAVE", "ERRONEA");
              ticksTemp = 300;
              estadoActual = (intentos >= 3) ? EST_BLOQUEADO : EST_ERROR_PASS;
            }
          }
        }
      }
      break;

    case EST_BLOQUEADO:
      setRGB(255, 0, 0);
      actualizarPantalla("SISTEMA", "BLOQUEADO");
      if (ticksTemp == 0 && intentos >= 3) { ticksTemp = 1000; intentos = 0; }
      if (ticksTemp == 0) resetLogin();
      break;

    case EST_BIENVENIDA:
    case EST_ERROR_PASS:
      if (ticksTemp == 0) {
        if (estadoActual == EST_BIENVENIDA) { ticksTemp = 1500; estadoActual = EST_MENU; }
        else resetLogin();
      }
      break;

    case EST_MENU:
      setRGB(0, 255, 0);
      mostrarMenu();
      if (key) ticksTemp = 1500;
      if (key == 'B') {
        strcpy(anguloStr, "000");
        posAng = 0;
        estadoActual = EST_MODO_B_INGRESO;
      }
      if (ticksTemp == 0) resetLogin();
      break;

    case EST_MODO_B_INGRESO:
      actualizarPantalla("MODO PROGRAMADO", anguloStr);
      if (key) {
        if (key >= '0' && key <= '9') {
          int digit = key - '0';
          anguloStr[posAng] = '0' + digit;
        } else if (key == '*') {
          if (posAng < 2) posAng++;
        } else if (key == 'D') {
          if (posAng > 0) { posAng--; anguloStr[posAng] = '0'; }
          else              anguloStr[0] = '0';
        }
      }
      if (btn1Pressed) {
        anguloObj = atoi(anguloStr);
        if (anguloObj > 180) {
          setRGB(255, 0, 0);
          ticksTemp = 300;
          estadoActual = EST_MODO_B_ERROR;
        } else {
          uint32_t cmp = CMP_MIN + ((CMP_MAX - CMP_MIN) * anguloObj) / 180;
          durezaX100   = (cmp * 10000UL) / (PWM_PERIOD - 1);
          duracionMs   = (anguloObj * 4000UL) / 180;
          ticksMov = 1;
          anguloAct = 0;
          estadoActual = EST_MODO_B_EJECUTANDO;
        }
      }
      break;

    case EST_MODO_B_EJECUTANDO:
      anguloAct = ticksMov / 5;
      if (anguloAct > anguloObj) anguloAct = anguloObj;
      moverServo(anguloAct);
      mostrarModoB(false);
      if (key == 'D') {
        anguloRetorno = anguloAct;
        ticksMov = 1;
        estadoActual = EST_MODO_B_RETORNO;
      } else if (anguloAct >= anguloObj) {
        moverServo(0);
        anguloAct = 0;
        ticksTemp = 1500;
        estadoActual = EST_MENU;
      }
      break;

    case EST_MODO_B_ERROR:
      mostrarModoB(true);
      if (key == 'D') {
        anguloRetorno = anguloAct;
        ticksMov = 1;
        estadoActual = EST_MODO_B_RETORNO;
      } else if (ticksTemp == 0) {
        estadoActual = EST_MODO_B_INGRESO;
      }
      break;

    case EST_MODO_B_RETORNO: {
      uint32_t elapsed = ticksMov / 5;
      if (elapsed >= anguloRetorno) {
        anguloAct = 0;
        moverServo(0);
        strcpy(anguloStr, "000");
        posAng = 0;
        estadoActual = EST_MODO_B_INGRESO;
      } else {
        anguloAct = anguloRetorno - elapsed;
        moverServo(anguloAct);
        char rbuf[12];
        sprintf(rbuf, "%d deg", (int)anguloAct);
        actualizarPantalla("RETORNANDO...", rbuf);
      }
      break;
    }
  }
}
