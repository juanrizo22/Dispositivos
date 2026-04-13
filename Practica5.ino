#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// OLED
#define SCREEN_W 128
#define SCREEN_H  64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);


#define RGB_PIN 48
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// Pines del display y botones
const int segPins[7] = {19, 21, 36, 35, 38, 20, 37};
const int digPins[4] = {2, 42, 41, 40};
const int LED1=15, LED2=16, BUZZER=3;
const int BTN1=4, BTN2=5, BTN3=6, BTN4=7;
const int SW1=13, SW2=14;

// 7-seg ánodo común: 0 = segmento encendido
const uint8_t SEG[10] = {
  0b1000000,0b1111001,0b0100100,0b0110000,0b0011001,
  0b0010010,0b0000010,0b1111000,0b0000000,0b0010000};
const uint8_t SEG_OFF = 0b1111111;
uint8_t dispBuf[4] = {SEG_OFF,SEG_OFF,SEG_OFF,SEG_OFF};
int digActual = 0;

// ── TIMER HARDWARE ──────────────────────────────────────────────────────────
hw_timer_t* timer0 = NULL;
uint32_t ticks = 0;            // equivalente a millis(), incrementa en loop
volatile bool flagTick = false; // bandera: el timer disparó (1ms)

void IRAM_ATTR onTimer() {
  flagTick = true; 
}
// ────────────────────────────────────────────────────────────────────────────

// Estados: 0=menú 1=cfg_t 2=cfg_r 3=resumen 4=terapia 5=fin
int estado = 0;
bool oledNuevo = true;

// Debounce (150ms)
bool antB1=1, antB2=1, antB3=1, antB4=1;
uint32_t tB1=0, tB2=0, tB3=0, tB4=0;
const uint32_t DB = 150;

// Doble clic en BTN1
int dcClics = 0;
uint32_t dcPrimero = 0;

// Configuración de terapia
int cfgTiempo = 180; // segundos (3 min por defecto)
int cfgReps   = 5;

// Estado de terapia
int totTiempo=0, totReps=0, repsHechas=0, razonFin=0;
uint32_t tInicio=0, acumPausa=0, tPausa=0;
bool enPausa = false;

// Aviso de 15 segundos restantes
bool avisoActivado=false, avisoActivo=false;
uint32_t tAvisoInicio=0, tAvisoFlash=0;
bool avisoFlashOn = false;

// Temporizadores de buzzer y RGB (en ticks)
uint32_t tBuzFin=0, tRgbFin=0;
uint32_t colorRGB = 0;

// Para detección de flanco en SW1 y anti-parpadeo OLED
bool antSW1 = HIGH;
int lastSlOled = -1;

// ── SALIDAS ─────────────────────────────────────────────────────────────────

// Activa buzzer + RGB de inmediato; los apaga cuando venza el temporizador
void feedback(uint32_t color, uint32_t ms) {
  uint32_t ahora = ticks;
  ledcWrite(BUZZER, 128);          // 50% duty = volumen reducido
  tBuzFin = ahora + ms;
  rgb.setPixelColor(0, color);
  rgb.show();
  colorRGB = color;
  tRgbFin  = ahora + ms;
}

// Apaga buzzer/RGB cuando venció el tiempo
void actualizarSalidas() {
  uint32_t ahora = ticks;
  if (tBuzFin && ahora >= tBuzFin) { ledcWrite(BUZZER, 0); tBuzFin = 0; }
  if (tRgbFin && ahora >= tRgbFin) { rgb.setPixelColor(0,0); rgb.show(); tRgbFin = 0; }
}

// ── DISPLAY 7-SEG ────────────────────────────────────────────────────────────

void apagarDisplay() {
  for (int i=0;i<4;i++) digitalWrite(digPins[i], LOW);
}

// Multiplexado: llamar cada 1ms (disparado por flagMux)
void mux() {
  // Apagar dígito anterior
  digitalWrite(digPins[digActual], LOW);
  digActual = (digActual + 1) % 4;
  // Escribir segmentos del dígito actual
  for (int s=0;s<7;s++) digitalWrite(segPins[s], (dispBuf[digActual]>>s)&1);
  // Encender dígito (ánodo común: HIGH = encendido)
  digitalWrite(digPins[digActual], HIGH);
}

void mostrarTiempo(int s) {
  int m = s / 60; s = s % 60;
  dispBuf[0] = SEG[m/10];
  dispBuf[1] = SEG[m%10];
  dispBuf[2] = SEG[s/10];
  dispBuf[3] = SEG[s%10];
}

void mostrarReps(int r) {
  dispBuf[0] = SEG_OFF;
  dispBuf[1] = SEG_OFF;
  dispBuf[2] = SEG[r/10];
  dispBuf[3] = SEG[r%10];
}

// ── TIEMPO RESTANTE ──────────────────────────────────────────────────────────

// Calcula segundos restantes descontando pausas acumuladas
int segsRestantes() {
  uint32_t ahora = ticks;
  uint32_t pausaTotal = acumPausa + (enPausa ? (ahora - tPausa) : 0);
  int transcurrido = (int)((ahora - tInicio - pausaTotal) / 1000);
  int restante = totTiempo - transcurrido;
  return restante < 0 ? 0 : restante;
}

// ── PANTALLAS OLED ───────────────────────────────────────────────────────────

void pantallaModos() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(20,0);  oled.print("TERAPIA FISICA");
  oled.setCursor(0,20);  oled.print("> Isotonica");
  oled.setCursor(0,35);  oled.print("  Stretching");
  oled.setCursor(0,55);  oled.print("BTN1:sel BTN2:sig");
  oled.display();
}

void pantallaCfgTiempo() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(10,0);  oled.print("TIEMPO DE TERAPIA");
  oled.setTextSize(2);
  oled.setCursor(35,25); oled.print(cfgTiempo/60); oled.print(" min");
  oled.setTextSize(1);
  oled.setCursor(0,55);  oled.print("BTN2:-  BTN3:+  BTN1:ok");
  oled.display();
}

void pantallaCfgReps() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(20,0);  oled.print("REPETICIONES");
  oled.setTextSize(2);
  oled.setCursor(50,25); oled.print(cfgReps);
  oled.setTextSize(1);
  oled.setCursor(0,55);  oled.print("BTN2:-  BTN3:+  BTN1:ok");
  oled.display();
}

void pantallaResumen() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(30,0);  oled.print("RESUMEN");
  oled.setCursor(0,15);  oled.print("Modo: Isotonica");
  oled.setCursor(0,28);  oled.print("Tiempo: "); oled.print(cfgTiempo/60); oled.print(" min");
  oled.setCursor(0,41);  oled.print("Reps:   "); oled.print(cfgReps);
  oled.setCursor(0,55);  oled.print("Doble-clic BTN1: INICIAR");
  oled.display();
}

void pantallaTerapia(int sl) {
  int sr = segsRestantes();
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  if (enPausa) {
    oled.setCursor(40,0); oled.print("PAUSA");
  } else {
    oled.setCursor(30,0); oled.print("TERAPIA");
  }
  if (sl == 0) {
    // Modo tiempo: muestra minutos:segundos
    oled.setTextSize(2);
    int m = sr/60, s = sr%60;
    oled.setCursor(20,20);
    if (m<10) oled.print("0"); oled.print(m);
    oled.print(":");
    if (s<10) oled.print("0"); oled.print(s);
    oled.setTextSize(1);
    oled.setCursor(0,50); oled.print("SW2:reps  SW1:pausa");
  } else {
    // Modo reps
    oled.setTextSize(2);
    oled.setCursor(30,20); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
    oled.setTextSize(1);
    oled.setCursor(0,50); oled.print("BTN3:rep  SW1:pausa");
  }
  oled.display();
}

void pantallaAviso() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(10,10); oled.print("! 15 SEGUNDOS !");
  oled.setCursor(10,30); oled.print("Prepare el fin");
  oled.display();
}

void pantallaFin() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(25,0);  oled.print("FIN TERAPIA");
  const char* razones[] = {"Tiempo","Reps","Manual"};
  oled.setCursor(0,20);  oled.print("Razon: "); oled.print(razones[razonFin]);
  oled.setCursor(0,35);  oled.print("Reps: "); oled.print(repsHechas);
  oled.setCursor(0,55);  oled.print("BTN4: volver al menu");
  oled.display();
}

// ── RESET Y FIN ──────────────────────────────────────────────────────────────

void volverAlMenu() {
  estado=0; oledNuevo=true;
  repsHechas=0; enPausa=false;
  acumPausa=0; tPausa=0; tInicio=0;
  avisoActivado=false; avisoActivo=false;
  dcClics=0;
  for (int i=0;i<4;i++) dispBuf[i]=SEG_OFF;
  apagarDisplay();
  ledcWrite(BUZZER,0);
  rgb.setPixelColor(0,0); rgb.show();
  lastSlOled=-1;
}

void terminarTerapia(int razon) {
  razonFin = razon;
  estado = 5; oledNuevo = true;
  apagarDisplay();
  enPausa = false;
  feedback(rgb.Color(255,0,0), 2000); // rojo 2s
}

// ── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
  // Segmentos y dígitos
  for (int i=0;i<7;i++) { pinMode(segPins[i],OUTPUT); digitalWrite(segPins[i],HIGH); }
  for (int i=0;i<4;i++) { pinMode(digPins[i],OUTPUT); digitalWrite(digPins[i],LOW); }

  // LEDs, buzzer, botones, switches
  pinMode(LED1,OUTPUT); pinMode(LED2,OUTPUT);
  pinMode(BTN1,INPUT_PULLUP); pinMode(BTN2,INPUT_PULLUP);
  pinMode(BTN3,INPUT_PULLUP); pinMode(BTN4,INPUT_PULLUP);
  pinMode(SW1,INPUT_PULLUP);  pinMode(SW2,INPUT_PULLUP);

  // Buzzer PWM (ESP32 core 3.x)
  ledcAttach(BUZZER, 1000, 8); // 1kHz, 8-bit
  ledcWrite(BUZZER, 0);

  // I2C rápido para reducir bloqueo del OLED
  Wire.begin();
  Wire.setClock(800000);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay(); oled.display();

  // RGB
  rgb.begin(); rgb.setBrightness(80); rgb.show();

  // ── Timer hardware a 1ms ──────────────────────────────────────────────────
  // timerBegin(frecuencia_hz): devuelve puntero al timer configurado
  timer0 = timerBegin(1000000);          // reloj base 1 MHz
  timerAttachInterrupt(timer0, &onTimer);
  timerAlarm(timer0, 1000, true, 0);     // alarma cada 1000 ticks = 1ms, autoreload
  // ─────────────────────────────────────────────────────────────────────────
}

// ── LOOP ─────────────────────────────────────────────────────────────────────

void loop() {
  // Procesar tick: incrementar contador y disparar mux
  if (flagTick) {
    flagTick = false;
    ticks++;                          // operacion fuera de la ISR
    if (estado == 4) mux();           // display activo solo en terapia
  }

  // Tomar snapshot del contador
  uint32_t ahora = ticks;

  // Apagar buzzer/RGB cuando vence su temporizador
  actualizarSalidas();

  // ── Lectura de botones con debounce ────────────────────────────────────────
  bool b1 = digitalRead(BTN1);
  bool b2 = digitalRead(BTN2);
  bool b3 = digitalRead(BTN3);
  bool b4 = digitalRead(BTN4);
  bool sw1 = digitalRead(SW1);
  bool sw2 = digitalRead(SW2);

  // Flanco de bajada en cada botón (HIGH→LOW)
  bool pulB1 = (!b1 && antB1 && (ahora-tB1)>DB);
  bool pulB2 = (!b2 && antB2 && (ahora-tB2)>DB);
  bool pulB3 = (!b3 && antB3 && (ahora-tB3)>DB);
  bool pulB4 = (!b4 && antB4 && (ahora-tB4)>DB);

  if (pulB1) tB1=ahora; if (pulB2) tB2=ahora;
  if (pulB3) tB3=ahora; if (pulB4) tB4=ahora;
  antB1=b1; antB2=b2; antB3=b3; antB4=b4;

  // Detección de doble clic en BTN1
  bool dobleClicB1 = false;
  if (pulB1) {
    if (dcClics==0) { dcClics=1; dcPrimero=ahora; }
    else if (dcClics==1 && (ahora-dcPrimero)<=500) { dobleClicB1=true; dcClics=0; }
    else { dcClics=1; dcPrimero=ahora; }
  }
  if (dcClics==1 && (ahora-dcPrimero)>500) dcClics=0;

  // ── Máquina de estados ────────────────────────────────────────────────────

  // Estado 0: Menú de modos
  if (estado == 0) {
    if (oledNuevo) { pantallaModos(); oledNuevo=false; }
    if (pulB1) { estado=1; oledNuevo=true; feedback(rgb.Color(0,255,0),100); }
  }

  // Estado 1: Configurar tiempo (1–5 min)
  else if (estado == 1) {
    if (oledNuevo) { pantallaCfgTiempo(); oledNuevo=false; }
    if (pulB2 && cfgTiempo>60)  { cfgTiempo-=60; oledNuevo=true; }
    if (pulB3 && cfgTiempo<300) { cfgTiempo+=60; oledNuevo=true; }
    if (pulB1) { estado=2; oledNuevo=true; feedback(rgb.Color(0,255,0),100); }
  }

  // Estado 2: Configurar repeticiones (3–12)
  else if (estado == 2) {
    if (oledNuevo) { pantallaCfgReps(); oledNuevo=false; }
    if (pulB2 && cfgReps>3)  { cfgReps--; oledNuevo=true; }
    if (pulB3 && cfgReps<12) { cfgReps++; oledNuevo=true; }
    if (pulB1) { estado=3; oledNuevo=true; feedback(rgb.Color(0,255,0),100); }
  }

  // Estado 3: Resumen — esperar doble clic para iniciar
  else if (estado == 3) {
    if (oledNuevo) { pantallaResumen(); oledNuevo=false; }
    if (dobleClicB1) {
      totTiempo  = cfgTiempo;
      totReps    = cfgReps;
      repsHechas = 0;
      enPausa    = false;
      acumPausa  = 0;
      tInicio    = ticks;           // usar ticks directo para precisión
      avisoActivado = false;
      avisoActivo   = false;
      antSW1 = digitalRead(SW1);    // capturar estado actual de SW1
      estado = 4; oledNuevo = true;
      mostrarTiempo(totTiempo);
      feedback(rgb.Color(0,0,255), 500); // azul 0.5s al iniciar
    }
  }

  // Estado 4: Terapia en curso
  else if (estado == 4) {
    int sl = (sw2==LOW) ? 1 : 0;   // 0=tiempo, 1=reps

    // Actualizar display 7-seg según modo
    if (!enPausa) {
      if (sl==0) mostrarTiempo(segsRestantes());
      else       mostrarReps(repsHechas);
    }

    // Pausa / reanuda con flanco de SW1
    if (sw1 != antSW1) {
      antSW1 = sw1;
      if (sw1 == LOW) {                       // SW1 → pausa
        enPausa = true;
        tPausa  = ticks;
        apagarDisplay();
        feedback(rgb.Color(255,165,0), 200);  // naranja
      } else {                                 // SW1 → reanudar
        acumPausa += (ticks - tPausa);
        enPausa = false;
        feedback(rgb.Color(0,255,0), 200);    // verde
      }
    }

    if (!enPausa) {
      int sr = segsRestantes();

      // Aviso 15 segundos: activar una sola vez
      if (!avisoActivado && sr <= 15 && sr > 0) {
        avisoActivado = true;
        avisoActivo   = true;
        tAvisoInicio  = ahora;
        tAvisoFlash   = ahora;
        avisoFlashOn  = true;
        feedback(rgb.Color(255,255,0), 500);  // amarillo
      }

      // Parpadeo del aviso durante 4s
      if (avisoActivo) {
        if ((ahora - tAvisoInicio) >= 4000) {
          avisoActivo = false;
          oledNuevo   = true;
        } else {
          if ((ahora - tAvisoFlash) >= 375) {
            tAvisoFlash  = ahora;
            avisoFlashOn = !avisoFlashOn;
            if (avisoFlashOn) pantallaAviso();
            else              { oled.clearDisplay(); oled.display(); }
          }
        }
      }

      // Redibujar OLED sólo si cambió el modo o hay contenido nuevo
      if (!avisoActivo && (sl != lastSlOled || oledNuevo)) {
        lastSlOled = sl;
        oledNuevo  = false;
        pantallaTerapia(sl);
      }

      // Contar rep con BTN3 (modo reps)
      if (sl==1 && pulB3) {
        repsHechas++;
        feedback(rgb.Color(0,255,0), 150);
        if (repsHechas >= totReps) terminarTerapia(1); // fin por reps
      }

      // Fin por tiempo
      if (sr <= 0) terminarTerapia(0);
    }

    // Salir con BTN4
    if (pulB4) terminarTerapia(2);
  }

  // Estado 5: Fin de terapia
  else if (estado == 5) {
    if (oledNuevo) { pantallaFin(); oledNuevo=false; }
    if (pulB4) volverAlMenu();
  }
}
