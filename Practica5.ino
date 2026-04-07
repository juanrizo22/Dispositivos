#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// ─── OLED ────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H  64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ─── RGB (⚠ pin 38; el 48 lo usa el segmento E) ─────────────
#define RGB_PIN 48
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ─── Pines ───────────────────────────────────────────────────
const int segPins[7] = {19, 21, 36, 35, 38, 20, 37}; // a b c d e f g
const int digPins[4] = {2, 42, 41, 40};
const int LED1=15, LED2=16, BUZZER=3;
const int BTN1=4, BTN2=5, BTN3=6, BTN4=7;
const int SW1=14, SW2=13;

// ─── 7-seg ánodo común (bit 0 = segmento ON) ─────────────────
const uint8_t SEG[10] = {
  0b1000000,0b1111001,0b0100100,0b0110000,0b0011001,
  0b0010010,0b0000010,0b1111000,0b0000000,0b0010000
};
const uint8_t SEG_OFF = 0b1111111;
uint8_t dispBuf[4] = {SEG_OFF,SEG_OFF,SEG_OFF,SEG_OFF};
unsigned long tMux=0; int digActual=0;

// ─── Máquina de estados ──────────────────────────────────────
// 0=menú  1=cfg tiempo  2=cfg reps  3=resumen  4=terapia  5=fin
int estado=0; bool oledNuevo=true;

// ─── Debounce botones ────────────────────────────────────────
bool antB1=1,antB2=1,antB3=1,antB4=1;
unsigned long tB1=0,tB2=0,tB3=0,tB4=0;
const unsigned long DB=150;

// ─── Doble clic BTN1 ─────────────────────────────────────────
int dcClics=0; unsigned long dcPrimero=0;

// ─── Configuración de la terapia ─────────────────────────────
int cfgTiempo=180, cfgReps=5;

// ─── Estado de la terapia ────────────────────────────────────
int totTiempo=0, totReps=0, repsHechas=0, razonFin=0;
unsigned long tInicio=0, acumPausa=0, tPausa=0;
bool enPausa=false;

// ─── Aviso 15 segundos ───────────────────────────────────────
bool avisoActivado=false, avisoActivo=false;
unsigned long tAvisoInicio=0, tAvisoFlash=0;
bool avisoFlashOn=false;

// ─── Buzzer / RGB one-shot ───────────────────────────────────
unsigned long tBuzFin=0, tRgbFin=0; uint32_t colorRGB=0;

// ─── Edge detection SW1 + anti-flicker OLED ──────────────────
bool antSW1=HIGH; int lastSlOled=-1;

// ═════════════════════════════════════════════════════════════
//  AUXILIARES
// ═════════════════════════════════════════════════════════════

// Activa buzzer Y rgb a la vez por 'ms' ms
void feedback(uint32_t color, unsigned long ms) {
  tBuzFin=millis()+ms; tRgbFin=millis()+ms; colorRGB=color;
  if (!avisoActivo) { ledcWrite(BUZZER,128); rgb.setPixelColor(0,color); rgb.show(); }
}

// Apaga buzzer/RGB cuando vence su timer
void actualizarSalidas(unsigned long ahora) {
  if (avisoActivo) return;
  if (ahora>=tBuzFin) ledcWrite(BUZZER,0);
  if (ahora>=tRgbFin) { rgb.setPixelColor(0,0); rgb.show(); }
}

// Multiplexado 7-seg: 1ms/dígito = 250Hz sin parpadeo visible
void mux(unsigned long ahora) {
  if (ahora-tMux < 1) return;
  tMux=ahora;
  digitalWrite(digPins[digActual], LOW);
  digActual=(digActual+1)%4;
  uint8_t p=dispBuf[digActual];
  for (int i=0;i<7;i++) digitalWrite(segPins[i],(p>>i)&1);
  digitalWrite(digPins[digActual], HIGH);
}

void apagarDisplay() { for (int i=0;i<4;i++) digitalWrite(digPins[i],LOW); }

void mostrarTiempo(int s) {
  dispBuf[0]=SEG_OFF; dispBuf[1]=SEG[s/60];
  dispBuf[2]=SEG[(s%60)/10]; dispBuf[3]=SEG[s%10];
}

void mostrarReps(int r) {
  dispBuf[0]=dispBuf[1]=SEG_OFF;
  dispBuf[2]=(r>=10)?SEG[r/10]:SEG_OFF; dispBuf[3]=SEG[r%10];
}

// Tiempo restante en segundos respetando pausas
int segsRestantes(unsigned long ahora) {
  unsigned long t = enPausa ? (tPausa-tInicio-acumPausa) : (ahora-tInicio-acumPausa);
  int s = totTiempo-(int)(t/1000);
  return (s<0)?0:s;
}

// Resetea todo y vuelve al menú
void volverAlMenu() {
  estado=0; oledNuevo=true;
  cfgTiempo=180; cfgReps=5; enPausa=false; repsHechas=0;
  avisoActivado=false; avisoActivo=false;
  tBuzFin=0; tRgbFin=0; dcClics=0; lastSlOled=-1;
  ledcWrite(BUZZER,0); rgb.setPixelColor(0,0); rgb.show();
  digitalWrite(LED1,LOW); digitalWrite(LED2,LOW);
  apagarDisplay();
}

// Termina la terapia: 1=reps completas  2=tiempo  3=BTN3
void terminarTerapia(int razon) {
  razonFin=razon; estado=5; avisoActivo=false;
  rgb.setPixelColor(0,0); rgb.show(); tRgbFin=0;
  tBuzFin=millis()+2000; ledcWrite(BUZZER,128);   // buzzer 2 segundos
  digitalWrite(LED1,LOW); digitalWrite(LED2,LOW);
  apagarDisplay(); oledNuevo=true;
}

// ═════════════════════════════════════════════════════════════
//  PANTALLAS OLED
// ═════════════════════════════════════════════════════════════

void pantallaModos() {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(18,2); oled.print("TERAPIA FISICA");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setCursor(5,22); oled.print("BTN1 > Isotonico");
  oled.setCursor(5,38); oled.print("BTN2 > Estiramiento");
  oled.display();
}

void pantallaCfgTiempo() {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(12,2); oled.print("Duracion de serie");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(26,18);
  oled.print(cfgTiempo/60); oled.print(":");
  if (cfgTiempo%60<10) oled.print("0"); oled.print(cfgTiempo%60);
  oled.setTextSize(1); oled.setCursor(2,50); oled.print("B1:-10s B2:+10s B3:OK");
  oled.display();
}

void pantallaCfgReps() {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(16,2); oled.print("Repeticiones");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setTextSize(3); oled.setCursor((cfgReps<10)?58:50,18); oled.print(cfgReps);
  oled.setTextSize(1); oled.setCursor(8,50); oled.print("B1:-1  B2:+1  B3:OK");
  oled.display();
}

void pantallaResumen() {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(22,2); oled.print("CONFIGURACION");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setCursor(5,18); oled.print("Duracion: ");
  oled.print(cfgTiempo/60); oled.print(":"); if(cfgTiempo%60<10)oled.print("0"); oled.print(cfgTiempo%60);
  oled.setCursor(5,30); oled.print("Reps:     "); oled.print(cfgReps);
  oled.drawLine(0,42,127,42,SSD1306_WHITE);
  oled.setCursor(5,46); oled.print("Doble clic BTN1");
  oled.setCursor(5,56); oled.print("para iniciar");
  oled.display();
}

void pantallaTerapia(int sl, bool pausada) {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(pausada?20:16,2); oled.print(pausada?"-- PAUSADA --":"EN PROGRESO");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setCursor(5,16); oled.print("Tiempo: ");
  oled.print(sl/60); oled.print(":"); if(sl%60<10)oled.print("0"); oled.print(sl%60);
  oled.setCursor(5,28); oled.print("Reps:   ");
  oled.print(repsHechas); oled.print("/"); oled.print(totReps);
  int bw=(totReps>0)?(repsHechas*116)/totReps:0;
  oled.drawRect(6,40,116,8,SSD1306_WHITE);
  if (bw>0) oled.fillRect(6,40,bw,8,SSD1306_WHITE);
  oled.setCursor(5,54); oled.print("2xB1=rep  B3=stop");
  oled.display();
}

void pantallaAviso(int sl) {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(16,2); oled.print("! ATENCION !");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setCursor(5,17); oled.print("Quedan 15 segundos");
  oled.setCursor(5,30); oled.print("Tiempo: ");
  oled.print(sl/60); oled.print(":"); if(sl%60<10)oled.print("0"); oled.print(sl%60);
  oled.setCursor(5,42); oled.print("Reps: "); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
  oled.display();
}

void pantallaFin() {
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(18,2); oled.print("FIN DE SESION");
  oled.drawLine(0,12,127,12,SSD1306_WHITE);
  oled.setCursor(5,16); oled.print(razonFin==1?"Reps completadas":
                                   razonFin==2?"Tiempo agotado":"Interrumpida");
  oled.setCursor(5,28); oled.print("Logradas: "); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
  oled.setCursor(5,40); oled.print("Logro:    "); oled.print((totReps>0)?(repsHechas*100)/totReps:0); oled.print("%");
  oled.setCursor(5,54); oled.print("BTN4 para salir");
  oled.display();
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
  for (int i=0;i<7;i++) { pinMode(segPins[i],OUTPUT); digitalWrite(segPins[i],HIGH); }
  for (int i=0;i<4;i++) { pinMode(digPins[i],OUTPUT); digitalWrite(digPins[i],LOW);  }
  pinMode(LED1,OUTPUT); pinMode(LED2,OUTPUT); pinMode(BUZZER,OUTPUT);
  pinMode(BTN1,INPUT_PULLUP); pinMode(BTN2,INPUT_PULLUP);
  pinMode(BTN3,INPUT_PULLUP); pinMode(BTN4,INPUT_PULLUP);
  pinMode(SW1, INPUT_PULLUP); pinMode(SW2, INPUT_PULLUP);
  ledcAttach(BUZZER,1000,8);               // PWM 1kHz, duty 128/255 = ~50% volumen
  rgb.begin(); rgb.clear(); rgb.show();
  Wire.begin(8,9); Wire.setClock(800000);  // 800kHz → menos parpadeo en 7-seg
  oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  oled.clearDisplay(); oled.display();
  antSW1=digitalRead(SW1);
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════
void loop() {
  unsigned long ahora=millis();
  if (estado==4) mux(ahora);  // 7-seg solo activo en terapia

  bool actB1=digitalRead(BTN1), actB2=digitalRead(BTN2);
  bool actB3=digitalRead(BTN3), actB4=digitalRead(BTN4);

  // BTN4: salir al menú desde cualquier estado (no desde el 0)
  if (estado!=0 && antB4==HIGH && actB4==LOW && (ahora-tB4>DB)) {
    volverAlMenu(); tB4=ahora;
  }
  antB4=actB4;

  // ── 0: Menú de selección ──────────────────────────────────
  if (estado==0) {
    if (oledNuevo) { pantallaModos(); oledNuevo=false; }
    if (antB1==HIGH && actB1==LOW && (ahora-tB1>DB)) {
      estado=1; cfgTiempo=180; oledNuevo=true; tB1=ahora;
    }
    antB1=actB1;
  }

  // ── 1: Configurar duración (1–5 min) ─────────────────────
  else if (estado==1) {
    if (oledNuevo) { pantallaCfgTiempo(); oledNuevo=false; }
    if (antB1==HIGH && actB1==LOW && (ahora-tB1>DB)) {
      if (cfgTiempo>60)  { cfgTiempo-=10; oledNuevo=true; }
      else               { feedback(rgb.Color(255,0,0),1000); }
      tB1=ahora;
    }
    if (antB2==HIGH && actB2==LOW && (ahora-tB2>DB)) {
      if (cfgTiempo<300) { cfgTiempo+=10; oledNuevo=true; }
      else               { feedback(rgb.Color(255,0,0),1000); }
      tB2=ahora;
    }
    if (antB3==HIGH && actB3==LOW && (ahora-tB3>DB)) {
      estado=2; cfgReps=5; oledNuevo=true; tB3=ahora;
    }
    antB1=actB1; antB2=actB2; antB3=actB3;
  }

  // ── 2: Configurar repeticiones (3–12) ────────────────────
  else if (estado==2) {
    if (oledNuevo) { pantallaCfgReps(); oledNuevo=false; }
    if (antB1==HIGH && actB1==LOW && (ahora-tB1>DB)) {
      if (cfgReps>3)  { cfgReps--; oledNuevo=true; }
      else            { feedback(rgb.Color(255,0,0),1000); }
      tB1=ahora;
    }
    if (antB2==HIGH && actB2==LOW && (ahora-tB2>DB)) {
      if (cfgReps<12) { cfgReps++; oledNuevo=true; }
      else            { feedback(rgb.Color(255,0,0),1000); }
      tB2=ahora;
    }
    if (antB3==HIGH && actB3==LOW && (ahora-tB3>DB)) {
      estado=3; oledNuevo=true; dcClics=0; tB3=ahora;
    }
    antB1=actB1; antB2=actB2; antB3=actB3;
  }

  // ── 3: Resumen + doble clic BTN1 para iniciar ────────────
  else if (estado==3) {
    if (oledNuevo) { pantallaResumen(); oledNuevo=false; }
    if (antB1==HIGH && actB1==LOW && (ahora-tB1>DB)) {
      if (dcClics==0 || (ahora-dcPrimero)>500) {
        dcClics=1; dcPrimero=ahora;           // primer clic
      } else {
        totTiempo=cfgTiempo; totReps=cfgReps;
        repsHechas=0; tInicio=ahora; acumPausa=0;
        antSW1=digitalRead(SW1);              // estado inicial de SW1
        enPausa=(antSW1==LOW);
        tPausa=ahora;
        digitalWrite(LED1, enPausa?HIGH:LOW);
        avisoActivado=false; avisoActivo=false; lastSlOled=-1;
        estado=4; oledNuevo=true; dcClics=0;
      }
      tB1=ahora;
    }
    antB1=actB1;
  }

  // ── 4: Terapia en progreso ────────────────────────────────
  else if (estado==4) {
    bool sw1=digitalRead(SW1), sw2=digitalRead(SW2);

    // SW1 cambió de posición → pausar o reanudar
    if (sw1!=antSW1) {
      antSW1=sw1;
      if (sw1==LOW) {                         // posición 0 → pausar
        enPausa=true; tPausa=ahora; digitalWrite(LED1,HIGH);
      } else {                                // posición 1 → reanudar
        if (enPausa) { acumPausa+=(ahora-tPausa); enPausa=false; }
        digitalWrite(LED1,LOW);
      }
      oledNuevo=true;
    }

    // SW2 controla LED2 y qué mostrar en el 7-seg
    if (sw2==LOW) { digitalWrite(LED2,HIGH); mostrarReps(repsHechas); }
    else          { digitalWrite(LED2,LOW);  mostrarTiempo(segsRestantes(ahora)); }

    int sl=segsRestantes(ahora);

    // Disparar aviso a los 15 s (solo una vez, no mientras está pausado)
    if (!avisoActivado && !enPausa && sl<=15 && sl>0) {
      avisoActivado=true; avisoActivo=true;
      tAvisoInicio=ahora; tAvisoFlash=ahora; avisoFlashOn=false;
      oledNuevo=true;
    }

    // Flash amarillo 1.33Hz (375ms on/off) durante 4 segundos
    if (avisoActivo) {
      if (ahora-tAvisoInicio>=4000) {
        avisoActivo=false; ledcWrite(BUZZER,0);
        rgb.setPixelColor(0,0); rgb.show(); oledNuevo=true;
      } else if (ahora-tAvisoFlash>=375) {
        avisoFlashOn=!avisoFlashOn; tAvisoFlash=ahora;
        ledcWrite(BUZZER,avisoFlashOn?128:0);
        rgb.setPixelColor(0,avisoFlashOn?rgb.Color(255,200,0):0); rgb.show();
      }
    }

    // Doble clic BTN1 → +1 repetición
    if (antB1==HIGH && actB1==LOW && (ahora-tB1>DB)) {
      if (dcClics==0 || (ahora-dcPrimero)>500) {
        dcClics=1; dcPrimero=ahora;
      } else {
        repsHechas++; dcClics=0;
        feedback(rgb.Color(0,0,255),800); oledNuevo=true;
      }
      tB1=ahora;
    }
    antB1=actB1;

    // BTN3 → interrumpir sesión
    if (antB3==HIGH && actB3==LOW && (ahora-tB3>DB)) {
      terminarTerapia(3); tB3=ahora;
    }
    antB3=actB3;

    // Condiciones de fin automático
    if      (estado==4 && repsHechas>=totReps) terminarTerapia(1);
    else if (estado==4 && sl<=0)               terminarTerapia(2);

    // OLED: redibujar solo cuando cambia el tiempo (reduce parpadeo 7-seg)
    if (estado==4 && (oledNuevo || sl!=lastSlOled)) {
      lastSlOled=sl;
      if (avisoActivo) pantallaAviso(sl); else pantallaTerapia(sl,enPausa);
      oledNuevo=false;
    }
  }

  // ── 5: Fin de sesión ──────────────────────────────────────
  else if (estado==5) {
    if (oledNuevo) { pantallaFin(); oledNuevo=false; }
  }

  actualizarSalidas(millis());
}
