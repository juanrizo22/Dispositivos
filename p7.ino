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
#define PWM_PERIOD   16384
const uint32_t CMP_MIN = 228;
const uint32_t CMP_MAX = 2148;
static mcpwm_cmpr_handle_t servoComparator = NULL;

// --- ESTADOS ---
enum Estado {
  EST_LOGIN, EST_BLOQUEADO, EST_BIENVENIDA, EST_ERROR_PASS,
  EST_MENU,
  EST_MODO_B_INGRESO,
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

// --- VARIABLES MODO B ---
// Ingreso de ángulo como entero acumulado (D borra, * avanza cursor no aplica)
int anguloIngresado = 0;   // valor numérico acumulado durante ingreso
int digitCount      = 0;   // cantidad de dígitos ingresados (max 3)

uint32_t anguloObj  = 0;
uint32_t anguloAct  = 0;
uint32_t durezaObj  = 0;   // calculada: (anguloObj * 100) / 180
uint32_t duracionObj = 0;  // calculada: (anguloObj * 4000) / 180  [ms → ticks /10]

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
  oled.setTextSize(1); oled.setCursor(0,  0); oled.println(t1);
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

// Pantalla de confirmación: muestra los 3 parámetros calculados
void mostrarConfirmacion(bool error) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);  oled.println("MODO PROGRAMADO");

  if (error) {
    oled.setCursor(0, 16); oled.println("ANGULO: ERROR");
    oled.setCursor(0, 28); oled.println("DUREZA: - - -");
    oled.setCursor(0, 40); oled.println("DURACION: - - -");
  } else {
    char buf[22];
    sprintf(buf, "ANGULO: %d grados", (int)anguloObj);
    oled.setCursor(0, 16); oled.println(buf);
    sprintf(buf, "DUREZA: %d %%", (int)durezaObj);
    oled.setCursor(0, 28); oled.println(buf);
    sprintf(buf, "DURACION: %d ms", (int)duracionObj);
    oled.setCursor(0, 40); oled.println(buf);
    oled.setCursor(0, 52); oled.println("BTN1: INICIAR");
  }
  oled.display();
}

void resetModoB() {
  anguloIngresado = 0;
  digitCount      = 0;
  estadoActual    = EST_MODO_B_INGRESO;
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

    // ── LOGIN ─────────────────────────────────────────────────────────────────
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

    // ── MENÚ ──────────────────────────────────────────────────────────────────
    case EST_MENU:
      setRGB(0, 255, 0);
      mostrarMenu();
      if (key == 'B') resetModoB();
      break;

    // ── INGRESO ÁNGULO ─────────────────────────────────────────────────────────
    case EST_MODO_B_INGRESO: {
      // Muestra el valor acumulado mientras se escribe
      char buf[5];
      if (digitCount == 0)
        sprintf(buf, "___");          // todavía no se ingresó nada
      else
        sprintf(buf, "%d", anguloIngresado);

      actualizarPantalla("ANGULO (0-180)", buf);

      if (key) {
        if (key >= '0' && key <= '9') {
          if (digitCount < 3) {            // máximo 3 dígitos (0-999, validamos en #)
            int digito = key - '0';        // convierte char '5' → int 5
            anguloIngresado = anguloIngresado * 10 + digito;
            digitCount++;
          }
        } else if (key == 'D') {
          // D borra todo el valor ingresado
          anguloIngresado = 0;
          digitCount      = 0;
        } else if (key == '#') {
          // Confirmar con BTN1 o '#': validar y calcular
          anguloObj = (uint32_t)anguloIngresado;
          if (anguloObj > 180) {
            // Muestra error en formato solicitado y vuelve a ingresar
            mostrarConfirmacion(true);
            ticksTemp    = 300;
            estadoRetorno = EST_MODO_B_INGRESO;
            estadoActual  = EST_MODO_B_ERROR;
          } else {
            // Calcula dureza y duración a partir del ángulo
            durezaObj   = (anguloObj * 100UL) / 180;          // %
            duracionObj = (anguloObj * 4000UL) / 180;         // ms
            estadoActual = EST_MODO_B_CONFIRMAR;
          }
        }
      }
      break;
    }

    // ── CONFIRMACIÓN ──────────────────────────────────────────────────────────
    case EST_MODO_B_CONFIRMAR:
      mostrarConfirmacion(false);
      if (digitalRead(BTN1_PIN) == LOW) {
        ticksMov  = 1;
        anguloAct = 0;
        estadoActual = EST_MODO_B_EJECUTANDO;
      }
      break;

    // ── EJECUTANDO ────────────────────────────────────────────────────────────
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
        ticksMov  = 0;
        // duracionObj está en ms → convertir a ticks de 10 ms
        ticksTemp = duracionObj / 10;
        estadoActual = EST_MODO_B_ESPERA;
      }
      break;
    }

    // ── ESPERA ────────────────────────────────────────────────────────────────
    case EST_MODO_B_ESPERA: {
      char buf[12];
      sprintf(buf, "%d deg", (int)anguloObj);
      actualizarPantalla("MANTENIENDO", buf);
      if (ticksTemp == 0) estadoActual = EST_MENU;
      break;
    }

    // ── ERROR MODO B ──────────────────────────────────────────────────────────
    case EST_MODO_B_ERROR:
      if (ticksTemp == 0) {
        anguloIngresado = 0;   // limpia para re-ingreso
        digitCount      = 0;
        estadoActual    = estadoRetorno;
      }
      break;
  }
}

