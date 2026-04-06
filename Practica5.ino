#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// ═══════════════════════════════════════════════════════════════
//  HARDWARE
// ═══════════════════════════════════════════════════════════════
#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ⚠ NeoPixel movido a pin 38: pin 48 lo ocupa el segmento E
#define RGB_PIN 48
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// Display 7 segmentos (ánodo común) – conexiones físicas correctas
const int segPins[7] = {19, 21, 36, 35, 38, 20, 37}; 
const int digPins[4] = {2, 42, 41, 40};
// Periféricos
const int LED1   = 15;   // indicador pausa
const int LED2   = 16;   // indicador modo visualización
const int BUZZER = 3;    // PWM via LEDC (50% duty = ~50% volumen)

// Botones y switches (INPUT_PULLUP → LOW = activo)
const int BTN1=4, BTN2=5, BTN3=6, BTN4=7;
const int SW1=13, SW2=14;

// ═══════════════════════════════════════════════════════════════
//  7 SEGMENTOS – ánodo común (bit=0 → segmento ON)
// ═══════════════════════════════════════════════════════════════
const uint8_t SEG[10] = {
  0b1000000, // 0
  0b1111001, // 1
  0b0100100, // 2
  0b0110000, // 3
  0b0011001, // 4
  0b0010010, // 5
  0b0000010, // 6
  0b1111000, // 7
  0b0000000, // 8
  0b0010000  // 9
};
const uint8_t SEG_OFF = 0b1111111;

uint8_t       dispBuf[4] = {SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF};
unsigned long tMux       = 0;
int           digActual  = 0;

// ═══════════════════════════════════════════════════════════════
//  MÁQUINA DE ESTADOS
//  0=selección  1=cfg_tiempo  2=cfg_reps  3=resumen
//  4=terapia    5=fin
// ═══════════════════════════════════════════════════════════════
int  estado    = 0;
bool oledNuevo = true;

// ═══════════════════════════════════════════════════════════════
//  DEBOUNCE
// ═══════════════════════════════════════════════════════════════
bool          antB1=1, antB2=1, antB3=1, antB4=1;
unsigned long tB1=0,   tB2=0,   tB3=0,   tB4=0;
const unsigned long DB = 150;

// ═══════════════════════════════════════════════════════════════
//  DOBLE CLIC BTN1
// ═══════════════════════════════════════════════════════════════
int           dcClics   = 0;
unsigned long dcPrimero = 0;

// ═══════════════════════════════════════════════════════════════
//  CONFIGURACIÓN TERAPIA
// ═══════════════════════════════════════════════════════════════
int cfgTiempo = 180;
int cfgReps   = 5;

// ═══════════════════════════════════════════════════════════════
//  ESTADO TERAPIA
// ═══════════════════════════════════════════════════════════════
int           totTiempo=0, totReps=0, repsHechas=0;
unsigned long tInicio=0, acumPausa=0, tPausa=0;
bool          enPausa  = false;
int           razonFin = 0;   // 1=reps  2=tiempo  3=BTN3

// ═══════════════════════════════════════════════════════════════
//  AVISO 15 SEGUNDOS
// ═══════════════════════════════════════════════════════════════
bool          avisoActivado=false, avisoActivo=false;
unsigned long tAvisoInicio=0, tAvisoFlash=0;
bool          avisoFlashOn = false;

// ═══════════════════════════════════════════════════════════════
//  BUZZER / RGB – timers
// ═══════════════════════════════════════════════════════════════
unsigned long tBuzFin  = 0;
unsigned long tRgbFin  = 0;
uint32_t      colorRGB = 0;

unsigned long tOled = 0;

// ═══════════════════════════════════════════════════════════════
//  AUXILIARES
// ═══════════════════════════════════════════════════════════════

// Apaga físicamente todos los dígitos del display
void apagarDisplay() {
  for (int i=0; i<4; i++) digitalWrite(digPins[i], LOW);
}

// Enciende buzzer al 50% duty (≈50% amplitud) por 'ms' ms
void encenderBuz(unsigned long ms) {
  tBuzFin = millis() + ms;
  if (!avisoActivo) ledcWrite(BUZZER, 128);   // 128/255 = 50% duty
}

// Enciende RGB inmediatamente y guarda cuándo apagarlo
void encenderRGB(uint32_t color, unsigned long ms) {
  colorRGB = color;
  tRgbFin  = millis() + ms;
  if (!avisoActivo) { rgb.setPixelColor(0, color); rgb.show(); }
}

// Solo apaga buzzer y RGB cuando vence su timer
void actualizarBuzRGB(unsigned long ahora) {
  if (avisoActivo) return;
  if (ahora >= tBuzFin) ledcWrite(BUZZER, 0);
  if (ahora >= tRgbFin) { rgb.setPixelColor(0, 0); rgb.show(); }
}

// Multiplexado – solo llamar cuando el display debe estar activo
void actualizarMux(unsigned long ahora) {
  if (ahora - tMux < 2) return;
  tMux = ahora;
  digitalWrite(digPins[digActual], LOW);
  digActual = (digActual + 1) % 4;
  uint8_t p = dispBuf[digActual];
  for (int i = 0; i < 7; i++) digitalWrite(segPins[i], (p >> i) & 1);
  digitalWrite(digPins[digActual], HIGH);
}

// 7-seg: [ blank | M | SS_dec | SS_uni ]
void mostrarTiempo(int segs) {
  dispBuf[0] = SEG_OFF;
  dispBuf[1] = SEG[segs / 60];
  dispBuf[2] = SEG[(segs % 60) / 10];
  dispBuf[3] = SEG[segs % 10];
}

void mostrarReps(int reps) {
  dispBuf[0] = SEG_OFF;
  dispBuf[1] = SEG_OFF;
  dispBuf[2] = (reps >= 10) ? SEG[reps / 10] : SEG_OFF;
  dispBuf[3] = SEG[reps % 10];
}

int segsRestantes(unsigned long ahora) {
  unsigned long transcurrido;
  if (enPausa) transcurrido = tPausa - tInicio - acumPausa;
  else         transcurrido = ahora  - tInicio - acumPausa;
  int s = totTiempo - (int)(transcurrido / 1000);
  return (s < 0) ? 0 : s;
}

void volverAlMenu() {
  estado=0; oledNuevo=true;
  cfgTiempo=180; cfgReps=5;
  enPausa=false; repsHechas=0;
  avisoActivado=false; avisoActivo=false;
  tBuzFin=0; tRgbFin=0; dcClics=0;
  ledcWrite(BUZZER, 0);
  rgb.setPixelColor(0, 0); rgb.show();
  digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
  apagarDisplay();
}

// ═══════════════════════════════════════════════════════════════
//  PANTALLAS OLED
// ═══════════════════════════════════════════════════════════════

void pantallaModos() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(18, 2);  oled.print("TERAPIA FISICA");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  oled.setCursor(5, 22);  oled.print("BTN1 > Isotonico");
  oled.setCursor(5, 38);  oled.print("BTN2 > Estiramiento");
  oled.display();
}

void pantallaCfgTiempo() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(12, 2);  oled.print("Duracion de serie");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  int m = cfgTiempo/60, s = cfgTiempo%60;
  oled.setTextSize(2);
  oled.setCursor(26, 18);
  oled.print(m); oled.print(":");
  if (s < 10) oled.print("0");
  oled.print(s);
  oled.setTextSize(1);
  oled.setCursor(2, 50);  oled.print("B1:-10s B2:+10s B3:OK");
  oled.display();
}

void pantallaCfgReps() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(16, 2);  oled.print("Repeticiones");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  oled.setTextSize(3);
  oled.setCursor((cfgReps < 10) ? 58 : 50, 18);
  oled.print(cfgReps);
  oled.setTextSize(1);
  oled.setCursor(8, 50);  oled.print("B1:-1  B2:+1  B3:OK");
  oled.display();
}

void pantallaResumen() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(22, 2);  oled.print("CONFIGURACION");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  int m = cfgTiempo/60, s = cfgTiempo%60;
  oled.setCursor(5, 18);  oled.print("Duracion:  ");
  oled.print(m); oled.print(":"); if (s<10) oled.print("0"); oled.print(s);
  oled.setCursor(5, 30);  oled.print("Reps:      "); oled.print(cfgReps);
  oled.drawLine(0, 42, 127, 42, SSD1306_WHITE);
  oled.setCursor(5, 46);  oled.print("Doble clic BTN1");
  oled.setCursor(5, 56);  oled.print("para iniciar");
  oled.display();
}

void pantallaTerapia(int sl, bool pausada) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(pausada ? 20 : 16, 2);
  oled.print(pausada ? "-- PAUSADA --" : "EN PROGRESO");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  int m = sl/60, s = sl%60;
  oled.setCursor(5, 16);  oled.print("Tiempo: ");
  oled.print(m); oled.print(":"); if (s<10) oled.print("0"); oled.print(s);
  oled.setCursor(5, 28);  oled.print("Reps:   ");
  oled.print(repsHechas); oled.print(" / "); oled.print(totReps);
  int ancho = (totReps > 0) ? (repsHechas * 116) / totReps : 0;
  oled.drawRect(6, 40, 116, 8, SSD1306_WHITE);
  if (ancho > 0) oled.fillRect(6, 40, ancho, 8, SSD1306_WHITE);
  oled.setCursor(5, 54);  oled.print("2xB1=rep  B3=stop");
  oled.display();
}

void pantallaAviso(int sl) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(16, 2);  oled.print("! ATENCION !");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  oled.setCursor(5, 17);  oled.print("Quedan 15 segundos");
  int m = sl/60, s = sl%60;
  oled.setCursor(5, 30);  oled.print("Tiempo: ");
  oled.print(m); oled.print(":"); if (s<10) oled.print("0"); oled.print(s);
  oled.setCursor(5, 42);  oled.print("Reps:   ");
  oled.print(repsHechas); oled.print(" / "); oled.print(totReps);
  oled.display();
}

void pantallaFin() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(18, 2);  oled.print("FIN DE SESION");
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  const char* razon = (razonFin==1) ? "Reps completadas" :
                      (razonFin==2) ? "Tiempo agotado"  :
                                      "Interrumpida";
  oled.setCursor(5, 16);  oled.print(razon);
  oled.setCursor(5, 28);  oled.print("Logradas: ");
  oled.print(repsHechas); oled.print("/"); oled.print(totReps);
  int pct = (totReps > 0) ? (repsHechas * 100) / totReps : 0;
  oled.setCursor(5, 40);  oled.print("Logro:    "); oled.print(pct); oled.print("%");
  oled.setCursor(5, 54);  oled.print("BTN4 para salir");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  // Segmentos: todos apagados (ánodo común: HIGH = segmento OFF)
  for (int i=0; i<7; i++) { pinMode(segPins[i], OUTPUT); digitalWrite(segPins[i], HIGH); }
  // Dígitos: todos apagados
  for (int i=0; i<4; i++) { pinMode(digPins[i], OUTPUT); digitalWrite(digPins[i], LOW); }

  pinMode(LED1,   OUTPUT); pinMode(LED2,   OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP); pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP); pinMode(BTN4, INPUT_PULLUP);
  pinMode(SW1,  INPUT_PULLUP); pinMode(SW2,  INPUT_PULLUP);

  // LEDC: 1 kHz, resolución 8 bits → duty 128 = 50% volumen
  ledcAttach(BUZZER, 1000, 8);

  rgb.begin(); rgb.clear(); rgb.show();
  Wire.begin(8, 9);   // SDA=8, SCL=9
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay(); oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  unsigned long ahora = millis();

  // 7-seg: SOLO activo durante terapia (estado 4)
  if (estado == 4) actualizarMux(ahora);

  bool actB1=digitalRead(BTN1), actB2=digitalRead(BTN2);
  bool actB3=digitalRead(BTN3), actB4=digitalRead(BTN4);

  // ── BTN4 global: volver al inicio (bloqueado en estado 0) ───
  if (estado != 0 && antB4==HIGH && actB4==LOW && (ahora-tB4 > DB)) {
    volverAlMenu();   // incluye apagarDisplay()
    tB4 = ahora;
  }
  antB4 = actB4;

  // ──────────────────────────────────────────────────────────
  //  ESTADO 0 – Selección de modo
  // ──────────────────────────────────────────────────────────
  if (estado == 0) {
    if (oledNuevo) { pantallaModos(); oledNuevo=false; }

    if (antB1==HIGH && actB1==LOW && (ahora-tB1 > DB)) {
      estado=1; cfgTiempo=180; oledNuevo=true;
      tB1=ahora;
    }
    // BTN2 → Estiramiento: pendiente laboratorio 16 abril
    antB1=actB1;
  }

  // ──────────────────────────────────────────────────────────
  //  ESTADO 1 – Configurar duración (60 s – 300 s)
  // ──────────────────────────────────────────────────────────
  else if (estado == 1) {
    if (oledNuevo) { pantallaCfgTiempo(); oledNuevo=false; }

    if (antB1==HIGH && actB1==LOW && (ahora-tB1 > DB)) {
      if (cfgTiempo > 60)  { cfgTiempo-=10; oledNuevo=true; }
      else                 { encenderBuz(1000); encenderRGB(rgb.Color(255,0,0), 1000); }
      tB1=ahora;
    }
    if (antB2==HIGH && actB2==LOW && (ahora-tB2 > DB)) {
      if (cfgTiempo < 300) { cfgTiempo+=10; oledNuevo=true; }
      else                 { encenderBuz(1000); encenderRGB(rgb.Color(255,0,0), 1000); }
      tB2=ahora;
    }
    if (antB3==HIGH && actB3==LOW && (ahora-tB3 > DB)) {
      estado=2; cfgReps=5; oledNuevo=true;
      tB3=ahora;
    }
    antB1=actB1; antB2=actB2; antB3=actB3;
  }

  // ──────────────────────────────────────────────────────────
  //  ESTADO 2 – Configurar repeticiones (3 – 12)
  // ──────────────────────────────────────────────────────────
  else if (estado == 2) {
    if (oledNuevo) { pantallaCfgReps(); oledNuevo=false; }

    if (antB1==HIGH && actB1==LOW && (ahora-tB1 > DB)) {
      if (cfgReps > 3)  { cfgReps--; oledNuevo=true; }
      else              { encenderBuz(1000); encenderRGB(rgb.Color(255,0,0), 1000); }
      tB1=ahora;
    }
    if (antB2==HIGH && actB2==LOW && (ahora-tB2 > DB)) {
      if (cfgReps < 12) { cfgReps++; oledNuevo=true; }
      else              { encenderBuz(1000); encenderRGB(rgb.Color(255,0,0), 1000); }
      tB2=ahora;
    }
    if (antB3==HIGH && actB3==LOW && (ahora-tB3 > DB)) {
      estado=3; oledNuevo=true; dcClics=0;
      tB3=ahora;
    }
    antB1=actB1; antB2=actB2; antB3=actB3;
  }

  // ──────────────────────────────────────────────────────────
  //  ESTADO 3 – Resumen + esperar doble clic BTN1
  // ──────────────────────────────────────────────────────────
  else if (estado == 3) {
    if (oledNuevo) { pantallaResumen(); oledNuevo=false; }

    if (antB1==HIGH && actB1==LOW && (ahora-tB1 > DB)) {
      if (dcClics==0 || (ahora - dcPrimero) > 500) {
        dcClics=1; dcPrimero=ahora;
      } else {
        totTiempo=cfgTiempo; totReps=cfgReps; repsHechas=0;
        tInicio=ahora; acumPausa=0; enPausa=false;
        avisoActivado=false; avisoActivo=false;
        estado=4; oledNuevo=true; dcClics=0;
        // display se activa solo: actualizarMux() corre cuando estado==4
      }
      tB1=ahora;
    }
    antB1=actB1;
  }

  // ──────────────────────────────────────────────────────────
  //  ESTADO 4 – Terapia en progreso
  // ──────────────────────────────────────────────────────────
  else if (estado == 4) {
    bool sw1 = digitalRead(SW1);
    bool sw2 = digitalRead(SW2);

    // SW1: LOW=pausar, HIGH=reanudar
    if (sw1 == LOW && !enPausa) {
      enPausa=true; tPausa=ahora;
      digitalWrite(LED1, HIGH);
      oledNuevo=true;
    } else if (sw1 == HIGH && enPausa) {
      enPausa=false; acumPausa += (ahora - tPausa);
      digitalWrite(LED1, LOW);
      oledNuevo=true;
    }

    // SW2 controla LED2 y qué se muestra en 7-seg
    if (sw2 == LOW) {
      digitalWrite(LED2, HIGH);
      mostrarReps(repsHechas);      // SW2=0 → repeticiones
    } else {
      digitalWrite(LED2, LOW);
      mostrarTiempo(segsRestantes(ahora));  // SW2=1 → tiempo M:SS
    }

    int sl = segsRestantes(ahora);

    // ── Aviso 15 segundos ─────────────────────────────────────
    if (!avisoActivado && !enPausa && sl <= 15 && sl > 0) {
      avisoActivado=true; avisoActivo=true;
      tAvisoInicio=ahora; tAvisoFlash=ahora; avisoFlashOn=false;
      oledNuevo=true;
    }

    // ── Flash 1.33 Hz (375 ms on/off) durante 4 segundos ─────
    if (avisoActivo) {
      if (ahora - tAvisoInicio >= 4000) {
        avisoActivo=false;
        ledcWrite(BUZZER, 0);
        rgb.setPixelColor(0, 0); rgb.show();
      } else if (ahora - tAvisoFlash >= 375) {
        avisoFlashOn = !avisoFlashOn;
        tAvisoFlash  = ahora;
        ledcWrite(BUZZER, avisoFlashOn ? 128 : 0);   // 50% duty en warning
        rgb.setPixelColor(0, avisoFlashOn ? rgb.Color(255,200,0) : 0);
        rgb.show();
      }
    }

    // ── Doble clic BTN1: +1 repetición ───────────────────────
    if (antB1==HIGH && actB1==LOW && (ahora-tB1 > DB)) {
      if (dcClics==0 || (ahora - dcPrimero) > 500) {
        dcClics=1; dcPrimero=ahora;
      } else {
        repsHechas++; dcClics=0;
        encenderBuz(800);
        encenderRGB(rgb.Color(0,0,255), 800);
        oledNuevo=true;
      }
      tB1=ahora;
    }
    antB1=actB1;

    // ── BTN3: interrumpir sesión ──────────────────────────────
    if (antB3==HIGH && actB3==LOW && (ahora-tB3 > DB)) {
      razonFin=3; estado=5;
      avisoActivo=false; ledcWrite(BUZZER, 0);
      encenderBuz(2000);
      digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
      apagarDisplay();   // ← 7-seg se apaga al salir del estado 4
      oledNuevo=true; tB3=ahora;
    }
    antB3=actB3;

    // ── Fin automático ────────────────────────────────────────
    if (repsHechas >= totReps) {
      razonFin=1; estado=5;
      avisoActivo=false; ledcWrite(BUZZER, 0);
      encenderBuz(2000);
      digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
      apagarDisplay();
      oledNuevo=true;
    } else if (sl <= 0) {
      razonFin=2; estado=5;
      avisoActivo=false; ledcWrite(BUZZER, 0);
      encenderBuz(2000);
      digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
      apagarDisplay();
      oledNuevo=true;
    }

    // ── OLED periódico (cada 500 ms o al cambiar algo) ────────
    if (estado == 4) {
      if (oledNuevo || (ahora - tOled >= 500)) {
        tOled = ahora;
        if (avisoActivo) pantallaAviso(sl);
        else             pantallaTerapia(sl, enPausa);
        oledNuevo = false;
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  //  ESTADO 5 – Fin de sesión
  // ──────────────────────────────────────────────────────────
  else if (estado == 5) {
    if (oledNuevo) { pantallaFin(); oledNuevo=false; }
    // BTN4 maneja la salida globalmente arriba
  }

  // ── Apagar buzzer/RGB cuando vence su timer ───────────────
  actualizarBuzRGB(millis());
}
