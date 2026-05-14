#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <Adafruit_NeoPixel.h>
#include "driver/mcpwm_prelude.h"

// ─── PINES ────────────────────────────────────────────────────────────────
#define SDA_PIN    2
#define SCL_PIN    1
#define PIN_RGB   48
#define SERVO_PIN 13
#define BTN1_PIN   4
#define BTN2_PIN   5
#define SW1_PIN   11
#define SW2_PIN   12
#define LED_PIN   46
#define POT_PIN   10

// ─── OLED ─────────────────────────────────────────────────────────────────
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ─── LED RGB ──────────────────────────────────────────────────────────────
Adafruit_NeoPixel pixel(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// ─── TECLADO ──────────────────────────────────────────────────────────────
const byte ROWS = 4, COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {6, 7, 15, 16};
byte colPins[COLS] = {17, 18, 8, 9};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─── MCPWM — SERVO SG90 ──────────────────────────────────────────────────
#define SERVO_RES_HZ   1000000UL
#define SERVO_PERIODO  20000UL

#define LEDC_RES_BITS      14UL
#define LEDC_MAX_COMPARE   16383
#define CMP_MIN            408UL
#define CMP_MAX            2008UL

#define SERVO_US_MIN       ((CMP_MIN * SERVO_PERIODO) / LEDC_MAX_COMPARE)
#define SERVO_US_MAX       ((CMP_MAX * SERVO_PERIODO) / LEDC_MAX_COMPARE)

static mcpwm_cmpr_handle_t servoCmp = NULL;

uint32_t anguloAus(uint32_t ang) {
  return SERVO_US_MIN + ang * (SERVO_US_MAX - SERVO_US_MIN) / 180UL;
}

uint32_t calcDureza(uint32_t us) {
  return us * 10000UL / SERVO_PERIODO;
}

void moverServo(uint32_t ang) {
  mcpwm_comparator_set_compare_value(servoCmp, anguloAus(ang));
}

void initServo() {
  mcpwm_timer_handle_t tmr = NULL;

  mcpwm_timer_config_t tc = {};
  tc.group_id = 0;
  tc.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
  tc.resolution_hz = SERVO_RES_HZ;
  tc.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
  tc.period_ticks = SERVO_PERIODO;
  mcpwm_new_timer(&tc, &tmr);

  mcpwm_oper_handle_t op = NULL;
  mcpwm_operator_config_t oc = {};
  oc.group_id = 0;
  mcpwm_new_operator(&oc, &op);
  mcpwm_operator_connect_timer(op, tmr);

  mcpwm_comparator_config_t cc = {};
  cc.flags.update_cmp_on_tez = true;
  mcpwm_new_comparator(op, &cc, &servoCmp);

  mcpwm_gen_handle_t gen = NULL;
  mcpwm_generator_config_t gc = {};
  gc.gen_gpio_num = SERVO_PIN;
  mcpwm_new_generator(op, &gc, &gen);

  mcpwm_generator_set_action_on_timer_event(
    gen,
    MCPWM_GEN_TIMER_EVENT_ACTION(
      MCPWM_TIMER_DIRECTION_UP,
      MCPWM_TIMER_EVENT_EMPTY,
      MCPWM_GEN_ACTION_HIGH
    )
  );

  mcpwm_generator_set_action_on_compare_event(
    gen,
    MCPWM_GEN_COMPARE_EVENT_ACTION(
      MCPWM_TIMER_DIRECTION_UP,
      servoCmp,
      MCPWM_GEN_ACTION_LOW
    )
  );

  mcpwm_comparator_set_compare_value(servoCmp, SERVO_US_MIN);
  mcpwm_timer_enable(tmr);
  mcpwm_timer_start_stop(tmr, MCPWM_TIMER_START_NO_STOP);
}

// ─── TIMERS CON INTERRUPCIONES ───────────────────────────────────────────
hw_timer_t *timerGral = NULL;
hw_timer_t *timerMov  = NULL;
hw_timer_t *timerBtn2 = NULL;

#define US_4S     4000000ULL
#define US_5S     5000000ULL
#define US_15S   15000000ULL
#define US_50MS     50000ULL
#define US_1S     1000000ULL

volatile bool flagTimerGral = false;
volatile bool flagTimerMov  = false;
volatile bool flagTimerBtn2 = false;

void IRAM_ATTR isrTimerGral() {
  flagTimerGral = true;
}

void IRAM_ATTR isrTimerMov() {
  flagTimerMov = true;
}

void IRAM_ATTR isrTimerBtn2() {
  flagTimerBtn2 = true;
}

bool tomarBandera(volatile bool &flag) {
  if (!flag) return false;
  noInterrupts();
  flag = false;
  interrupts();
  return true;
}

void iniciarTimerUnaVez(hw_timer_t *t, uint64_t us) {
  timerStop(t);
  timerWrite(t, 0);
  timerAlarm(t, us, false, 0);
  timerStart(t);
}

void iniciarTimerPeriodico(hw_timer_t *t, uint64_t us) {
  timerStop(t);
  timerWrite(t, 0);
  timerAlarm(t, us, true, 0);
  timerStart(t);
}

// ─── USUARIOS ────────────────────────────────────────────────────────────
const char* passwords[] = {"1204", "2206", "1110"};
const char* nombres[]   = {"RUBEN", "JUAN", "MANU"};
const int NUM_USERS = 3;

// ─── ESTADOS ─────────────────────────────────────────────────────────────
enum Estado {
  EST_LOGIN,
  EST_ERROR_PASS,
  EST_BLOQUEADO,
  EST_BIENVENIDA,
  EST_MENU,
  EST_MODO_A,
  EST_MODO_B_INGRESO,
  EST_MODO_B_MOVIENDO,
  EST_MODO_B_LISTO,
  EST_MODO_B_ERROR
};

enum SubModoA {
  MODO_A_POT,
  MODO_A_BTN
};

Estado estadoActual = EST_LOGIN;
Estado estadoAnterior = EST_MODO_B_ERROR;
SubModoA subModoA = MODO_A_POT;

// ─── VARIABLES ───────────────────────────────────────────────────────────
char passIngresada[5] = "****";
int posCursor = 0;
int intentos = 0;
char nombreActual[20] = "";

char anguloStr[4] = "000";
int posAng = 0;

uint32_t anguloObj = 0;
uint32_t anguloAct = 0;
uint32_t durezaX100 = 0;
uint32_t durUs = 0;
uint32_t pasosMov = 0;

uint16_t adcProm = 0;
uint16_t ticksAdc = 0;
uint16_t ticksBlink = 0;
bool ledBlinkOn = false;

bool btn1Ant = HIGH;
bool btn2Ant = HIGH;
bool btn2Activo = false;

bool btn1ActivoA = false;
bool btn1LargoA = false;

bool modoBNavReady = false;

// ─── FUNCIONES MODO A ────────────────────────────────────────────────────
uint16_t leerAdcPromedio12() {
  uint32_t suma = 0;
  for (int i = 0; i < 12; i++) {
    suma += analogRead(POT_PIN);
  }
  return suma / 12;
}

uint32_t adcAangulo(uint16_t adc) {
  return (uint32_t)adc * 180UL / 4095UL;
}

uint32_t duracionAangulo(uint64_t durUsBtn) {
  if (durUsBtn >= US_4S) return 180;
  return (uint32_t)(durUsBtn * 180ULL / US_4S);
}

const char* posicionBiomecanica(uint32_t ang) {
  if (ang <= 45) return "EXTENSION";
  if (ang <= 95) return "FLEX PARCIAL";
  if (ang <= 150) return "FLEX COMPLETA";
  return "HIPERFLEXION";
}

void colorPorAngulo(uint32_t ang, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (ang <= 45) {
    r = 0; g = 180; b = 180;
  } else if (ang <= 95) {
    r = 180; g = 0; b = 180;
  } else if (ang <= 150) {
    r = 255; g = 120; b = 0;
  } else {
    r = 255; g = 255; b = 255;
  }
}

uint8_t medioPeriodoBlinkTicks(uint32_t ang) {
  if (ang <= 45) return 30;     // 0.33 Hz aprox.
  if (ang <= 95) return 15;     // 0.66 Hz aprox.
  if (ang <= 150) return 10;    // 1 Hz
  return 5;                     // 2 Hz
}

void actualizarParpadeoModoA() {
  ticksBlink++;

  if (ticksBlink >= medioPeriodoBlinkTicks(anguloAct)) {
    ticksBlink = 0;
    ledBlinkOn = !ledBlinkOn;

    if (ledBlinkOn) {
      uint8_t r, g, b;
      colorPorAngulo(anguloAct, r, g, b);
      setRGB(r, g, b);
      digitalWrite(LED_PIN, HIGH);
    } else {
      setRGB(0, 0, 0);
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// ─── PANTALLAS ───────────────────────────────────────────────────────────
void pantallaLogin() {
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.print("BIENVENIDO");

  oled.setTextSize(2);
  oled.setCursor(16, 36);
  for (int i = 0; i < 4; i++) oled.print(passIngresada[i]);

  oled.display();
}

void pantallaMsg(const char* l1, const char* l2) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 8);
  oled.println(l1);

  oled.setTextSize(2);
  oled.setCursor(0, 28);
  oled.println(l2);

  oled.display();
}

void pantallaMenu() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("== MENU PRINCIPAL ==");
  oled.setCursor(0, 18);
  oled.println("A: MANUAL");
  oled.setCursor(0, 32);
  oled.println("B: PROGRAMADO");
  oled.setCursor(0, 50);
  oled.println("# cerrar sesion");
  oled.display();
}

void pantallaModoA() {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  if (subModoA == MODO_A_POT) oled.print("MODO A: POT");
  else oled.print("MODO A: BTN1");

  oled.setCursor(0, 13);
  oled.print("ANGULO: ");
  oled.print((int)anguloAct);
  oled.print("\xF8");

  oled.setCursor(0, 26);

  if (subModoA == MODO_A_BTN || digitalRead(SW1_PIN) == HIGH) {
    uint32_t durezaManual = calcDureza(anguloAus(anguloAct));
    oled.print("DUREZA: ");
    oled.print((unsigned)(durezaManual / 100));
    oled.print(".");
    uint32_t dec = durezaManual % 100;
    if (dec < 10) oled.print("0");
    oled.print((unsigned)dec);
    oled.print("%");
  } else {
    oled.print("ADC: ");
    oled.print((unsigned)adcProm);
  }

  oled.setCursor(0, 39);
  oled.print(posicionBiomecanica(anguloAct));

  oled.setCursor(0, 52);
  oled.print("# MENU");
  oled.display();
}

void pantallaModoB_Ingreso() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("MODO PROGRAMADO");
  oled.setCursor(0, 14);
  oled.println("Ingrese angulo:");

  oled.setTextSize(3);
  oled.setCursor(10, 32);
  oled.print(anguloStr);

  oled.setTextSize(2);
  oled.print("\xF8");
  oled.display();
}

void pantallaModoB_Error() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("MODO PROGRAMADO");
  oled.setCursor(0, 14);
  oled.println("ANGULO: ERROR");
  oled.setCursor(0, 28);
  oled.println("DUREZA:   ---");
  oled.setCursor(0, 42);
  oled.println("DURACION: ---");
  oled.display();
}

void pantallaModoB_Resultado(bool moviendo) {
  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print("MODO PROGRAMADO");

  if (moviendo) {
    oled.setCursor(96, 0);
    oled.print(">>>");
  }

  oled.setCursor(0, 14);
  oled.print("ANGULO: ");
  oled.print((int)anguloAct);
  oled.print("\xF8");

  oled.setCursor(0, 28);
  oled.print("DUREZA: ");
  oled.print((unsigned)(durezaX100 / 100));
  oled.print(".");
  uint32_t dec = durezaX100 % 100;
  if (dec < 10) oled.print("0");
  oled.print((unsigned)dec);
  oled.print("%");

  oled.setCursor(0, 42);
  oled.print("DURACION: ");
  uint32_t ms = durUs / 1000UL;
  oled.print((unsigned)(ms / 1000));
  oled.print(".");
  uint32_t cs = (ms % 1000) / 10;
  if (cs < 10) oled.print("0");
  oled.print((unsigned)cs);
  oled.print("s");

  oled.display();
}

// ─── RESET ───────────────────────────────────────────────────────────────
void resetLogin() {
  strcpy(passIngresada, "****");
  posCursor = 0;

  timerStop(timerGral);
  timerStop(timerMov);
  timerStop(timerBtn2);

  flagTimerGral = false;
  flagTimerMov = false;
  flagTimerBtn2 = false;

  btn2Activo = false;
  btn1ActivoA = false;
  btn1LargoA = false;

  digitalWrite(LED_PIN, LOW);
  setRGB(0, 0, 255);

  estadoAnterior = EST_MODO_B_ERROR;
  estadoActual = EST_LOGIN;

  pantallaLogin();
}

void resetModoB() {
  strcpy(anguloStr, "000");
  posAng = 0;
}

// ─── SETUP ───────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oled.setTextColor(WHITE);
  oled.cp437(true);

  pixel.begin();
  pixel.setBrightness(80);
  pixel.show();

  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(POT_PIN, INPUT);
  digitalWrite(LED_PIN, LOW);

  initServo();
  anguloAct = 0;

  timerGral = timerBegin(1000000);
  timerMov  = timerBegin(1000000);
  timerBtn2 = timerBegin(1000000);

  timerAttachInterrupt(timerGral, &isrTimerGral);
  timerAttachInterrupt(timerMov,  &isrTimerMov);
  timerAttachInterrupt(timerBtn2, &isrTimerBtn2);

  timerStop(timerGral);
  timerStop(timerMov);
  timerStop(timerBtn2);

  resetLogin();
}

// ─── LOOP ────────────────────────────────────────────────────────────────
void loop() {
  char key = keypad.getKey();

  bool btn1Now = digitalRead(BTN1_PIN);
  bool btn1Press = false;

  if (estadoActual != EST_MODO_A) {
    btn1Press = (btn1Ant == HIGH && btn1Now == LOW);
    btn1Ant = btn1Now;
  }

  bool btn2Now = digitalRead(BTN2_PIN);

  bool dentroSistema = (estadoActual == EST_MENU ||
                        estadoActual == EST_MODO_A ||
                        estadoActual == EST_MODO_B_INGRESO ||
                        estadoActual == EST_MODO_B_MOVIENDO ||
                        estadoActual == EST_MODO_B_LISTO ||
                        estadoActual == EST_MODO_B_ERROR);

  if (dentroSistema) {
    if (btn2Ant == HIGH && btn2Now == LOW) {
      flagTimerBtn2 = false;
      iniciarTimerUnaVez(timerBtn2, US_4S);
      btn2Activo = true;
    }

    if (btn2Ant == LOW && btn2Now == HIGH && btn2Activo) {
      timerStop(timerBtn2);
      flagTimerBtn2 = false;
      btn2Activo = false;
    }

    if (btn2Activo && tomarBandera(flagTimerBtn2)) {
      timerStop(timerMov);
      timerStop(timerBtn2);

      moverServo(0);
      anguloAct = 0;
      intentos = 0;
      btn2Activo = false;
      btn2Ant = btn2Now;

      resetLogin();
      return;
    }
  } else {
    if (btn2Activo) {
      timerStop(timerBtn2);
      flagTimerBtn2 = false;
      btn2Activo = false;
    }
  }

  btn2Ant = btn2Now;

  switch (estadoActual) {

    case EST_LOGIN:
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaLogin();
      }

      if (key) {
        if (key >= '0' && key <= '9') {
          passIngresada[posCursor] = key;
          pantallaLogin();

        } else if (key == '*') {
          if (posCursor < 3) posCursor++;
          pantallaLogin();

        } else if (key == 'B') {
          strcpy(passIngresada, "****");
          posCursor = 0;
          pantallaLogin();

        } else if (key == '#') {
          if (strchr(passIngresada, '*') == NULL) {
            int idx = -1;

            for (int i = 0; i < NUM_USERS; i++) {
              if (strcmp(passIngresada, passwords[i]) == 0) {
                idx = i;
                break;
              }
            }

            if (idx != -1) {
              intentos = 0;
              strncpy(nombreActual, nombres[idx], sizeof(nombreActual) - 1);
              nombreActual[sizeof(nombreActual) - 1] = '\0';

              setRGB(0, 255, 0);
              pantallaMsg("BIENVENIDO/A", nombreActual);

              flagTimerGral = false;
              iniciarTimerUnaVez(timerGral, US_4S);

              estadoAnterior = EST_LOGIN;
              estadoActual = EST_BIENVENIDA;

            } else {
              intentos++;
              strcpy(passIngresada, "****");
              posCursor = 0;

              if (intentos >= 3) {
                setRGB(255, 255, 0);
                pantallaMsg("SISTEMA", "BLOQUEADO");

                flagTimerGral = false;
                iniciarTimerUnaVez(timerGral, US_15S);

                estadoAnterior = EST_LOGIN;
                estadoActual = EST_BLOQUEADO;

              } else {
                setRGB(255, 0, 0);
                pantallaMsg("CLAVE", "ERRONEA");

                flagTimerGral = false;
                iniciarTimerUnaVez(timerGral, US_5S);

                estadoAnterior = EST_LOGIN;
                estadoActual = EST_ERROR_PASS;
              }
            }
          }
        }
      }
      break;

    case EST_ERROR_PASS:
      if (tomarBandera(flagTimerGral)) resetLogin();
      break;

    case EST_BLOQUEADO:
      if (tomarBandera(flagTimerGral)) {
        intentos = 0;
        resetLogin();
      }
      break;

    case EST_BIENVENIDA:
      if (tomarBandera(flagTimerGral)) {
        timerStop(timerGral);
        setRGB(0, 255, 0);
        pantallaMenu();

        flagTimerGral = false;
        iniciarTimerUnaVez(timerGral, US_15S);

        estadoAnterior = EST_BIENVENIDA;
        estadoActual = EST_MENU;
      }
      break;

    case EST_MENU:
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaMenu();
      }

      if (key) {
        flagTimerGral = false;
        iniciarTimerUnaVez(timerGral, US_15S);

        if (key == 'A') {
          timerStop(timerGral);
          flagTimerGral = false;

          subModoA = MODO_A_POT;
          adcProm = leerAdcPromedio12();
          anguloAct = adcAangulo(adcProm);
          moverServo(anguloAct);

          ticksAdc = 0;
          ticksBlink = 0;
          ledBlinkOn = false;
          btn1ActivoA = false;
          btn1LargoA = false;

          pantallaModoA();

          flagTimerMov = false;
          iniciarTimerPeriodico(timerMov, US_50MS);

          estadoAnterior = EST_MENU;
          estadoActual = EST_MODO_A;

        } else if (key == 'B') {
          timerStop(timerGral);
          flagTimerGral = false;

          resetModoB();
          pantallaModoB_Ingreso();

          estadoAnterior = EST_MENU;
          estadoActual = EST_MODO_B_INGRESO;

        } else if (key == '#') {
          intentos = 0;
          moverServo(0);
          anguloAct = 0;
          resetLogin();
        }
      }

      if (tomarBandera(flagTimerGral)) {
        intentos = 0;
        moverServo(0);
        anguloAct = 0;
        resetLogin();
      }
      break;

    case EST_MODO_A: {
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaModoA();
      }

      bool sw2Activo = (digitalRead(SW2_PIN) == LOW);

      if (sw2Activo && subModoA == MODO_A_POT) {
        subModoA = MODO_A_BTN;
        moverServo(0);
        anguloAct = 0;
        durUs = 0;
        btn1ActivoA = false;
        btn1LargoA = false;
        timerStop(timerBtn2);
        flagTimerBtn2 = false;
        pantallaModoA();
      } else if (!sw2Activo && subModoA == MODO_A_BTN) {
        subModoA = MODO_A_POT;
        btn1ActivoA = false;
        btn1LargoA = false;
        timerStop(timerBtn2);
        flagTimerBtn2 = false;
        adcProm = leerAdcPromedio12();
        anguloAct = adcAangulo(adcProm);
        moverServo(anguloAct);
        ticksAdc = 0;
        pantallaModoA();
      }

      if (tomarBandera(flagTimerMov)) {
        if (subModoA == MODO_A_POT) {
          ticksAdc++;

          if (ticksAdc >= 12) {
            ticksAdc = 0;
            adcProm = leerAdcPromedio12();
            anguloAct = adcAangulo(adcProm);
            moverServo(anguloAct);
            pantallaModoA();
          }

        } else {
          bool btn1NowA = digitalRead(BTN1_PIN);

          if (btn1Ant == HIGH && btn1NowA == LOW) {
            timerStop(timerBtn2);
            timerWrite(timerBtn2, 0);
            timerStart(timerBtn2);
            btn1ActivoA = true;
            btn1LargoA = false;
          }

          if (btn1ActivoA && !btn1LargoA && timerRead(timerBtn2) >= US_4S) {
            btn1LargoA = true;
            anguloAct = 180;
            durUs = US_4S;
            moverServo(anguloAct);
            pantallaModoA();
          }

          if (btn1Ant == LOW && btn1NowA == HIGH && btn1ActivoA) {
            timerStop(timerBtn2);

            uint64_t durBtn = timerRead(timerBtn2);
            if (durBtn > US_4S) durBtn = US_4S;

            durUs = (uint32_t)durBtn;
            anguloAct = duracionAangulo(durBtn);
            moverServo(anguloAct);

            btn1ActivoA = false;
            btn1LargoA = false;

            pantallaModoA();
          }

          btn1Ant = btn1NowA;
        }

        actualizarParpadeoModoA();
      }

      if (key == '#') {
        timerStop(timerMov);
        timerStop(timerBtn2);
        flagTimerMov = false;
        flagTimerBtn2 = false;

        digitalWrite(LED_PIN, LOW);
        setRGB(0, 255, 0);

        pantallaMenu();

        flagTimerGral = false;
        iniciarTimerUnaVez(timerGral, US_15S);

        estadoAnterior = EST_MODO_A;
        estadoActual = EST_MENU;
      }
      break;
    }

    case EST_MODO_B_INGRESO:
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaModoB_Ingreso();
      }

      if (key) {
        if (key >= '0' && key <= '9') {
          anguloStr[posAng] = key;
          pantallaModoB_Ingreso();

        } else if (key == '*') {
          posAng = (posAng < 2) ? posAng + 1 : 0;
          pantallaModoB_Ingreso();

        } else if (key == 'D') {
          if (posAng > 0) posAng--;
          anguloStr[posAng] = '0';
          pantallaModoB_Ingreso();

        } else if (key == '#') {
          resetModoB();
          pantallaMenu();

          flagTimerGral = false;
          iniciarTimerUnaVez(timerGral, US_15S);

          estadoAnterior = EST_MODO_B_INGRESO;
          estadoActual = EST_MENU;
        }
      }

      if (btn1Press) {
        anguloObj = (uint32_t)(anguloStr[0] - '0') * 100 +
                    (uint32_t)(anguloStr[1] - '0') * 10 +
                    (uint32_t)(anguloStr[2] - '0');

        if (anguloObj > 180) {
          setRGB(255, 0, 0);
          pantallaModoB_Error();

          estadoAnterior = EST_MODO_B_INGRESO;
          estadoActual = EST_MODO_B_ERROR;

        } else {
          durezaX100 = calcDureza(anguloAus(anguloObj));

          moverServo(0);
          anguloAct = 0;
          durUs = 0;
          pasosMov = 0;

          pantallaModoB_Resultado(true);

          if (anguloObj == 0) {
            pantallaModoB_Resultado(false);
            modoBNavReady = false;
            flagTimerMov = false;
            iniciarTimerUnaVez(timerMov, US_1S);
            estadoAnterior = EST_MODO_B_INGRESO;
            estadoActual = EST_MODO_B_LISTO;
          } else {
            flagTimerMov = false;
            iniciarTimerPeriodico(timerMov, US_50MS);

            estadoAnterior = EST_MODO_B_INGRESO;
            estadoActual = EST_MODO_B_MOVIENDO;
          }
        }
      }
      break;

    case EST_MODO_B_MOVIENDO:
      if (tomarBandera(flagTimerMov)) {
        pasosMov++;

        if (pasosMov > anguloObj) pasosMov = anguloObj;

        anguloAct = pasosMov;
        durUs = pasosMov * US_50MS;

        moverServo(anguloAct);
        pantallaModoB_Resultado(true);

        if (anguloAct >= anguloObj) {
          timerStop(timerMov);
          flagTimerMov = false;

          durUs = anguloObj * US_50MS;
          pantallaModoB_Resultado(false);

          modoBNavReady = false;
          iniciarTimerUnaVez(timerMov, US_1S);

          estadoAnterior = EST_MODO_B_MOVIENDO;
          estadoActual = EST_MODO_B_LISTO;
        }
      }

      if (key == 'D') {
        timerStop(timerMov);
        flagTimerMov = false;

        moverServo(0);
        anguloAct = 0;
        resetModoB();
        pantallaModoB_Ingreso();

        estadoAnterior = EST_MODO_B_MOVIENDO;
        estadoActual = EST_MODO_B_INGRESO;

      } else if (key == '#') {
        timerStop(timerMov);
        flagTimerMov = false;

        resetModoB();
        pantallaMenu();

        flagTimerGral = false;
        iniciarTimerUnaVez(timerGral, US_15S);

        estadoAnterior = EST_MODO_B_MOVIENDO;
        estadoActual = EST_MENU;
      }
      break;

    case EST_MODO_B_LISTO:
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaModoB_Resultado(false);
      }

      if (!modoBNavReady && tomarBandera(flagTimerMov)) {
        timerStop(timerMov);
        modoBNavReady = true;
      }

      if (modoBNavReady) {
        if (key == 'D') {
          moverServo(0);
          anguloAct = 0;
          resetModoB();
          pantallaModoB_Ingreso();

          estadoAnterior = EST_MODO_B_LISTO;
          estadoActual = EST_MODO_B_INGRESO;

        } else if (key == '#') {
          resetModoB();
          pantallaMenu();

          flagTimerGral = false;
          iniciarTimerUnaVez(timerGral, US_15S);

          estadoAnterior = EST_MODO_B_LISTO;
          estadoActual = EST_MENU;
        }
      }
      break;

    case EST_MODO_B_ERROR:
      if (estadoActual != estadoAnterior) {
        estadoAnterior = estadoActual;
        pantallaModoB_Error();
        setRGB(255, 0, 0);
      }

      if (key == 'D') {
        setRGB(0, 0, 0);
        moverServo(0);
        anguloAct = 0;
        resetModoB();
        pantallaModoB_Ingreso();

        estadoAnterior = EST_MODO_B_ERROR;
        estadoActual = EST_MODO_B_INGRESO;

      } else if (key == '#') {
        setRGB(0, 255, 0);
        resetModoB();
        pantallaMenu();

        flagTimerGral = false;
        iniciarTimerUnaVez(timerGral, US_15S);

        estadoAnterior = EST_MODO_B_ERROR;
        estadoActual = EST_MENU;
      }
      break;
  }
}
