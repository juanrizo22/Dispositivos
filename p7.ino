#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <Adafruit_NeoPixel.h>
#include "driver/mcpwm_prelude.h"

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
// compareValue = 228 + (ángulo × 1920) / 180
#define PWM_PERIOD   16384
const uint32_t CMP_MIN = 228;
const uint32_t CMP_MAX = 2148;

static mcpwm_cmpr_handle_t servoComparator = NULL;

// --- ESTADOS ---
enum Estado {
  EST_LOGIN, EST_BLOQUEADO, EST_BIENVENIDA, EST_ERROR_PASS,
  EST_MENU,
  EST_MODO_B_INGRESO,
  EST_MODO_B_DUREZA,
  EST_MODO_B_DURACION,
  EST_MODO_B_CONFIRMAR,
  EST_MODO_B_EJECUTANDO,
  EST_MODO_B_ESPERA,
  EST_MODO_B_ERROR
};
Estado estadoActual  = EST_LOGIN;
Estado estadoRetorno = EST_MODO_B_INGRESO;

// --- VARIABLES DE LOGIN ---
const char* passwords[] = {"1204", "2206", "1011"};
const char* nombres[]   = {"RUBEN", "JUAN", "MANU"};
char passIngresada[5]   = "****";
int posCursor = 0;
int intentos  = 0;

// --- VARIABLES MODO B (dígitos como int para usar key-'0') ---
int anguloDig[3]    = {0, 0, 0};
int posAng          = 0;
uint32_t anguloObj  = 0;
uint32_t anguloAct  = 0;

int durezaDig[3]    = {0, 0, 0};
int posDureza       = 0;
uint32_t durezaObj  = 0;

int duracionDig[3]     = {0, 0, 0};
int posDuracion        = 0;
uint32_t duracionObj   = 0;

// --- TIMERS ---
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

// --- UTILIDADES ---
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

int digsToNum(int* digs) {
  return digs[0] * 100 + digs[1] * 10 + digs[2];
}

void initServo() {
  mcpwm_timer_handle_t timer = NULL;

  mcpwm_timer_config_t timer_cfg;
  memset(&timer_cfg, 0, sizeof(timer_cfg));
  timer_cfg.group_id      = 0;
  timer_cfg.clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT;
  timer_cfg.resolution_hz = 819200;
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
  uint32_t cmp = CMP_MIN + (ang * 1920UL) / 180;
  mcpwm_comparator_set_compare_value(servoComparator, cmp);
}

// --- PANTALLAS ---
void resetLogin() {
  strcpy(passIngresada, "****");
  posCursor = 0;
  estadoActual = EST_LOGIN;
  setRGB(0, 0, 255);
}

void actualizarPantalla(const char* t1, const char* t2) {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setCursor(0,  0);  oled.println(t1);
  oled.setTextSize(2); oled.setCursor(30, 30); oled.println(t2);
  oled.display();
}

void mostrarMenu() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0,  0); oled.println("== MENU PRINCIPAL ==");
  oled.setCursor(0, 20); oled.println("A: MANUAL");
  oled.setCursor(0, 35); oled.println("B: PROGRAMADO");
  oled.display();
}

// Muestra ángulo, dureza y duración en líneas separadas antes de ejecutar
void mostrarParametros() {
  char buf[22];
  oled.clearDisplay();
  oled.setTextSize(1);
  sprintf(buf, "Angulo:  %3d grados", (int)anguloObj);
  oled.setCursor(0,  0); oled.println(buf);
  sprintf(buf, "Dureza:  %3d %%", (int)durezaObj);
  oled.setCursor(0, 16); oled.println(buf);
  sprintf(buf, "Duracion:%3d seg", (int)duracionObj);
  oled.setCursor(0, 32); oled.println(buf);
  oled.setCursor(0, 50); oled.println("BTN1: INICIAR");
  oled.display();
}

void resetModoB() {
  memset(anguloDig,   0, sizeof(anguloDig));   posAng      = 0;
  memset(durezaDig,   0, sizeof(durezaDig));   posDureza   = 0;
  memset(duracionDig, 0, sizeof(duracionDig)); posDuracion = 0;
  estadoActual = EST_MODO_B_INGRESO;
}

// --- SETUP ---
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
  timerAlarm(timerHw, 10000, true, 0); // tick cada 10 ms

  resetLogin();
}

// --- LOOP PRINCIPAL ---
void loop() {
  char key = keypad.getKey();

  // BTN2 largo → logout inmediato desde cualquier estado
  if (estadoActual != EST_LOGIN && ticksBtn2 >= 400) {
    ticksBtn2 = 0;
    moverServo(0);
    resetLogin();
    return;
  }

  switch (estadoActual) {

    // ── LOGIN ────────────────────────────────────────────────────────────────
    case EST_LOGIN:
      actualizarPantalla("INGRESE CLAVE", passIngresada);
      if (key) {
        if (key >= '0' && key <= '9') {
          passIngresada[posCursor] = key;       // guarda el char directamente
        } else if (key == '*') {
          if (posCursor < 3) posCursor++;
        } else if (key == 'D') {
          resetLogin();
        } else if (key == '#') {
          if (strchr(passIngresada, '*') == NULL) {
            int userIdx = -1;
            for (int i = 0; i < 3; i++)
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
        if (estadoActual == EST_BIENVENIDA) estadoActual = EST_MENU;
        else resetLogin();
      }
      break;

    // ── MENÚ ─────────────────────────────────────────────────────────────────
    case EST_MENU:
      setRGB(0, 255, 0);
      mostrarMenu();
      if (key == 'B') resetModoB();
      break;

    // ── INGRESO ÁNGULO ────────────────────────────────────────────────────────
    case EST_MODO_B_INGRESO: {
      char buf[4];
      sprintf(buf, "%d%d%d", anguloDig[0], anguloDig[1], anguloDig[2]);
      actualizarPantalla("ANGULO (0-180)", buf);
      if (key) {
        if (key >= '0' && key <= '9') {
          int numero = key - '0';          // convierte '1' → 1
          anguloDig[posAng] = numero;
        } else if (key == '*') {
          if (posAng < 2) posAng++;
        } else if (key == 'D') {          // D borra todo el valor ingresado
          memset(anguloDig, 0, sizeof(anguloDig));
          posAng = 0;
        } else if (key == '#') {
          anguloObj = digsToNum(anguloDig);
          if (anguloObj > 180) {
            setRGB(255, 0, 0);
            actualizarPantalla("ANGULO", "ERROR");
            ticksTemp = 300;
            estadoRetorno = EST_MODO_B_INGRESO;
            estadoActual  = EST_MODO_B_ERROR;
          } else {
            estadoActual = EST_MODO_B_DUREZA;
          }
        }
      }
      break;
    }

    // ── INGRESO DUREZA (0-100 %) ──────────────────────────────────────────────
    case EST_MODO_B_DUREZA: {
      char buf[4];
      sprintf(buf, "%d%d%d", durezaDig[0], durezaDig[1], durezaDig[2]);
      actualizarPantalla("DUREZA (0-100)", buf);
      if (key) {
        if (key >= '0' && key <= '9') {
          int numero = key - '0';
          durezaDig[posDureza] = numero;
        } else if (key == '*') {
          if (posDureza < 2) posDureza++;
        } else if (key == 'D') {
          memset(durezaDig, 0, sizeof(durezaDig));
          posDureza = 0;
        } else if (key == '#') {
          durezaObj = digsToNum(durezaDig);
          if (durezaObj > 100) {
            setRGB(255, 0, 0);
            actualizarPantalla("DUREZA", "ERROR");
            ticksTemp = 300;
            estadoRetorno = EST_MODO_B_DUREZA;
            estadoActual  = EST_MODO_B_ERROR;
          } else {
            estadoActual = EST_MODO_B_DURACION;
          }
        }
      }
      break;
    }

    // ── INGRESO DURACIÓN (0-999 seg) ──────────────────────────────────────────
    case EST_MODO_B_DURACION: {
      char buf[4];
      sprintf(buf, "%d%d%d", duracionDig[0], duracionDig[1], duracionDig[2]);
      actualizarPantalla("DURACION (seg)", buf);
      if (key) {
        if (key >= '0' && key <= '9') {
          int numero = key - '0';
          duracionDig[posDuracion] = numero;
        } else if (key == '*') {
          if (posDuracion < 2) posDuracion++;
        } else if (key == 'D') {
          memset(duracionDig, 0, sizeof(duracionDig));
          posDuracion = 0;
        } else if (key == '#') {
          duracionObj  = digsToNum(duracionDig);
          estadoActual = EST_MODO_B_CONFIRMAR;
        }
      }
      break;
    }

    // ── CONFIRMACIÓN: muestra ángulo, dureza y duración en líneas separadas ───
    case EST_MODO_B_CONFIRMAR:
      mostrarParametros();
      if (digitalRead(BTN1_PIN) == LOW) {
        ticksMov = 1;
        anguloAct = 0;
        estadoActual = EST_MODO_B_EJECUTANDO;
      }
      break;

    // ── EJECUTANDO: mueve el servo según dureza (velocidad) ──────────────────
    case EST_MODO_B_EJECUTANDO: {
      uint32_t vel = (durezaObj == 0) ? 1 : durezaObj;
      // dureza 100 → 1 grado cada 50 ms; dureza 50 → 1 grado cada 100 ms
      anguloAct = (ticksMov * vel) / 500;
      if (anguloAct > anguloObj) anguloAct = anguloObj;
      moverServo(anguloAct);

      char buf[12];
      sprintf(buf, "%d deg", (int)anguloAct);
      actualizarPantalla("EJECUTANDO...", buf);

      if (anguloAct >= anguloObj) {
        ticksMov  = 0;                        // detiene el contador de movimiento
        ticksTemp = duracionObj * 100;        // duracion en ticks (×10 ms)
        estadoActual = EST_MODO_B_ESPERA;
      }
      break;
    }

    // ── ESPERA: mantiene posición por la duración configurada ─────────────────
    case EST_MODO_B_ESPERA: {
      char buf[12];
      sprintf(buf, "%d deg", (int)anguloObj);
      actualizarPantalla("MANTENIENDO", buf);
      if (ticksTemp == 0) estadoActual = EST_MENU;
      break;
    }

    // ── ERROR MODO B ─────────────────────────────────────────────────────────
    case EST_MODO_B_ERROR:
      if (ticksTemp == 0) estadoActual = estadoRetorno;
      break;
  }
}

