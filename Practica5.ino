//https://openneuro.org/datasets/ds005366/versions/2.0.0
#include <Wire.h>               // Biblioteca I2C (para comunicar con la OLED)
#include <Adafruit_GFX.h>       // Biblioteca de gráficos base para la OLED
#include <Adafruit_SSD1306.h>   // Driver específico del controlador SSD1306 de la OLED
#include <Adafruit_NeoPixel.h>  // Biblioteca para controlar el LED RGB NeoPixel

// ─── CONFIGURACIÓN OLED Y RGB ────────────────────────────────────────
#define SCREEN_W 128          // Ancho de la pantalla OLED en píxeles
#define SCREEN_H  64          // Alto de la pantalla OLED en píxeles
#define OLED_ADDR 0x3C        // Dirección I2C de la OLED (estándar para SSD1306)
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1); // Objeto OLED: dimensiones, bus I2C, sin pin reset (-1)

#define RGB_PIN 48                                              // Pin de datos del NeoPixel
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);      // 1 LED, pin 48, formato GRB a 800kHz

// ─── PINES ───────────────────────────────────────────────────────────
const int segPins[7] = {19, 21, 36, 35, 38, 20, 37}; // Pines de segmentos a,b,c,d,e,f,g del 7-seg
const int pinDP = 47;                                  // Pin del punto decimal del 7-seg
const int digPins[4] = {2, 42, 41, 40};               // Pines de los 4 dígitos (ánodo común: HIGH=activo)
const int LED1=15, LED2=16, BUZZER=3;                  // LED1=indicador pausa, LED2=sigue SW2, BUZZER=PWM
const int BTN1=4, BTN2=5, BTN3=6, BTN4=7;             // Botones (INPUT_PULLUP → 0 cuando presionado)
const int SW1=14, SW2=13;                              // Switches (INPUT_PULLUP → 0 cuando activado)

// ─── 7-SEGMENTOS (ÁNODO COMÚN: 0 = SEGMENTO ENCENDIDO) ──────────────
// Cada byte representa los 7 segmentos: bit0=seg_a, bit1=seg_b, ..., bit6=seg_g
// Con ánodo común: 0 enciende el segmento, 1 lo apaga
const uint8_t SEG[10] = {
  0b1000000, // 0 → enciende a,b,c,d,e,f (apaga g)
  0b1111001, // 1 → enciende b,c
  0b0100100, // 2 → enciende a,b,d,e,g
  0b0110000, // 3 → enciende a,b,c,d,g
  0b0011001, // 4 → enciende b,c,f,g
  0b0010010, // 5 → enciende a,c,d,f,g
  0b0000010, // 6 → enciende a,c,d,e,f,g
  0b1111000, // 7 → enciende a,b,c
  0b0000000, // 8 → enciende todos los segmentos
  0b0010000  // 9 → enciende a,b,c,d,f,g
};
const uint8_t SEG_OFF = 0b1111111; // Apaga todos los segmentos (ánodo común: 1 = apagado)

// Variables del buffer de multiplexado — volatile porque las modifica una tarea de FreeRTOS
volatile uint8_t dispBuf[4] = {SEG_OFF, SEG_OFF, SEG_OFF, SEG_OFF}; // Buffer de 4 dígitos (índice 0=izquierda)
volatile bool dispDP[4] = {false, false, false, false};              // Buffer de puntos decimales por dígito
volatile int digActual = 0;          // Índice del dígito que se está mostrando ahora mismo
volatile bool tickMultiplex = false; // Bandera: el timer la pone en true cada 2.5ms

hw_timer_t * timerMultiplex = NULL;  // Handle del hardware timer (se inicializa en setup)

// ─── MÁQUINA DE ESTADOS ──────────────────────────────────────────────
int estado = 0;       // Estado actual de la máquina: 0=menú, 1=cfg tiempo, 2=cfg reps, 3=resumen, 4=terapia, 5=fin
bool oledNuevo = true; // true = hay que redibujar la OLED en este ciclo de loop
int cfgTiempo = 180, cfgReps = 5; // Valores configurados por el usuario (tiempo en segundos, reps)
int totTiempo=0, totReps=0;       // Valores copiados al iniciar la terapia (no cambian durante la sesión)
int repsHechas=0, razonFin=0;     // Contador de reps completadas; razón de fin: 1=completo, 2=tiempo, 3=manual
unsigned long tInicio=0;          // millis() en el momento de iniciar la terapia
unsigned long acumPausa=0;        // ms totales acumulados en pausa (se restan al calcular tiempo transcurrido)
unsigned long tPausa=0;           // millis() en el momento en que comenzó la pausa actual
bool enPausa=false;               // true = terapia pausada actualmente (SW1=LOW)

// Debounce de botones: cada BTN tiene timestamp del último disparo y estado anterior
unsigned long tB1=0, tB2=0, tB3=0, tB4=0; // Timestamps del último evento válido por botón
bool antB1=1, antB2=1, antB3=1, antB4=1;  // Estado anterior de cada botón (1=no presionado, INPUT_PULLUP)
const int DB = 150;                         // Tiempo de debounce en ms (ignora rebotes menores a 150ms)
unsigned long tUltimoClicB1 = 0;           // Timestamp del penúltimo clic de B1 (para detectar doble clic)

// Control de duración del buzzer y RGB (se apagan solos cuando millis supera estos valores)
unsigned long tBuzFin=0, tRgbFin=0;

// Variables del aviso de tiempo final (parpadeo en los últimos 15 segundos)
bool avisoActivo=false;       // true = estamos en modo parpadeo de aviso final
unsigned long tAvisoFlash=0;  // Timestamp del último cambio de estado del flash
bool avisoFlashOn=false;      // Estado actual del flash (true=encendido, false=apagado)
int lastSlOled = -1;          // Último valor de segundos mostrado en OLED (para evitar redibujos innecesarios)

// ═══════════════════════════════════════════════════════════════════
//  INTERRUPCIÓN DEL TIMER — SE EJECUTA EN IRAM (memoria rápida)
// ═══════════════════════════════════════════════════════════════════
void IRAM_ATTR onTimerMux() {
  tickMultiplex = true; // Solo pone la bandera — ninguna otra operación permitida en ISR (regla ESP-IDF)
}

// ═══════════════════════════════════════════════════════════════════
//  TAREA FREERTOS: MULTIPLEXADO DEL DISPLAY 7 SEGMENTOS
//  Corre en Core 1 con prioridad 5 (alta) para no perder ciclos
// ═══════════════════════════════════════════════════════════════════
void taskMultiplexado(void *pvParameters) {
  while (true) {                        // Bucle infinito de FreeRTOS (nunca termina)
    if (tickMultiplex) {                // Solo actúa cuando el timer disparó la bandera
      tickMultiplex = false;            // Limpia la bandera para el siguiente ciclo

      // PASO 1: apagar todos los dígitos antes de cambiar segmentos (evita "fantasmas" entre dígitos)
      for(int i=0; i<4; i++) digitalWrite(digPins[i], LOW);

      // PASO 2: avanzar al siguiente dígito (rueda circular 0→1→2→3→0...)
      digActual = (digActual + 1) % 4;

      // PASO 3: cargar los segmentos del dígito actual desde el buffer
      uint8_t p = dispBuf[digActual];                       // Byte de segmentos del dígito a mostrar
      for (int i=0; i<7; i++) digitalWrite(segPins[i], (p >> i) & 1); // Pone cada segmento según el bit

      // PASO 4: configurar el punto decimal (ánodo común: LOW = encendido)
      digitalWrite(pinDP, dispDP[digActual] ? LOW : HIGH);

      // PASO 5: activar el dígito actual (ánodo común: HIGH = dígito encendido)
      digitalWrite(digPins[digActual], HIGH);
    }
    vTaskDelay(1 / portTICK_PERIOD_MS); // Cede 1ms al scheduler (evita que el watchdog reinicie el ESP32)
  }
}

// ═══════════════════════════════════════════════════════════════════
//  FUNCIONES AUXILIARES
// ═══════════════════════════════════════════════════════════════════

// feedback: activa el buzzer y el RGB durante 'ms' milisegundos
// color: valor de color para NeoPixel (ej. rgb.Color(0,0,255) para azul)
void feedback(uint32_t color, int ms) {
  tBuzFin = millis() + ms;           // El buzzer se apaga cuando millis() llegue a este valor
  tRgbFin = millis() + ms;           // El RGB se apaga cuando millis() llegue a este valor
  ledcWrite(BUZZER, 128);            // Activa buzzer a 50% de duty cycle (volumen medio)
  rgb.setPixelColor(0, color);       // Establece el color del NeoPixel (índice 0 = único LED)
  rgb.show();                        // Envía los datos de color al LED físico
}

// apagarDisplay: pone todos los dígitos en blanco (SEG_OFF apaga todos los segmentos)
void apagarDisplay() {
  for(int i=0; i<4; i++) {
    dispBuf[i] = SEG_OFF; // Todos los segmentos apagados en este dígito
    dispDP[i] = false;    // Punto decimal apagado en este dígito
  }
}

// actualizarDisplayFisico: rellena dispBuf con tiempo o reps según el switch SW2
// segRestantes: segundos restantes de la terapia
// reps: número de repeticiones completadas
// modoTiempo: true = mostrar tiempo (SW2=HIGH), false = mostrar reps (SW2=LOW)
void actualizarDisplayFisico(int segRestantes, int reps, bool modoTiempo) {
  if (modoTiempo) {
    int m = segRestantes / 60;  // Parte entera de los minutos
    int s = segRestantes % 60;  // Segundos restantes dentro del minuto actual
    dispBuf[0] = SEG_OFF;       // Dígito 0 (izquierda) siempre apagado en formato M.SS
    dispBuf[1] = SEG[m % 10];   // Dígito 1 = unidades de minutos
    dispDP[1]  = true;          // Punto decimal en dígito 1 para separar M.SS
    dispBuf[2] = SEG[s / 10];   // Dígito 2 = decenas de segundos
    dispDP[2]  = false;         // Sin punto decimal
    dispBuf[3] = SEG[s % 10];   // Dígito 3 = unidades de segundos
  } else {
    dispDP[1] = false;                               // Sin punto decimal en modo reps
    dispBuf[0] = dispBuf[1] = SEG_OFF;              // Dígitos 0 y 1 apagados
    dispBuf[2] = (reps >= 10) ? SEG[reps/10] : SEG_OFF; // Decena de reps (apagado si <10)
    dispBuf[3] = SEG[reps % 10];                    // Unidades de reps (siempre visible)
  }
}

// terminarTerapia: finaliza la sesión activa y va al estado 5 (pantalla de fin)
// razon: 1=reps completadas, 2=tiempo agotado, 3=interrumpida con B3
void terminarTerapia(int razon) {
  razonFin = razon;         // Guarda por qué terminó para mostrarlo en estado 5
  estado = 5;               // Transición a estado de fin
  oledNuevo = true;         // Forzar redibujado de la OLED en estado 5
  apagarDisplay();          // Apaga todos los dígitos del 7-seg
  ledcWrite(BUZZER, 0);     // Asegura que el buzzer esté apagado antes de la notificación final
  rgb.setPixelColor(0, 0); rgb.show(); // Apaga el RGB
  tBuzFin = millis() + 2000; ledcWrite(BUZZER, 128); // Buzzer de fin durante 2 segundos
  digitalWrite(LED1, LOW);  // Apaga LED1 (indicador de pausa)
  digitalWrite(LED2, LOW);  // Apaga LED2 (indicador de SW2)
}

// drawHeader: limpia la OLED y dibuja el encabezado estándar de cada pantalla
// title: texto a mostrar como título (máx ~15 caracteres en tamaño 1)
void drawHeader(const char* title) {
  oled.clearDisplay();                              // Borra todo el framebuffer de la OLED
  oled.setTextSize(1);                              // Texto pequeño (6x8 px por carácter)
  oled.setTextColor(SSD1306_WHITE);                 // Color blanco (único disponible en OLED monocromática)
  oled.setCursor(15, 2);                            // Posición: ligeramente indentado, arriba
  oled.print(title);                                // Escribe el título
  oled.drawLine(0, 12, 127, 12, SSD1306_WHITE);     // Línea horizontal bajo el título (separador visual)
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP — Inicialización de hardware (se ejecuta una sola vez)
// ═══════════════════════════════════════════════════════════════════
void setup() {
  // Configurar todos los pines de segmentos como salida
  for (int i=0; i<7; i++) pinMode(segPins[i], OUTPUT);
  // Configurar pines de dígitos como salida
  for (int i=0; i<4; i++) pinMode(digPins[i], OUTPUT);
  // Configurar pin de punto decimal como salida y apagarlo (HIGH = apagado en ánodo común)
  pinMode(pinDP, OUTPUT); digitalWrite(pinDP, HIGH);
  // Configurar LEDs y buzzer como salidas
  pinMode(LED1, OUTPUT); pinMode(LED2, OUTPUT); pinMode(BUZZER, OUTPUT);
  // Configurar botones y switches como entradas con pull-up interno
  // (leer LOW = presionado, HIGH = suelto)
  pinMode(BTN1, INPUT_PULLUP); pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP); pinMode(BTN4, INPUT_PULLUP);
  pinMode(SW1, INPUT_PULLUP);  pinMode(SW2, INPUT_PULLUP);

  // Inicializar el hardware timer a frecuencia base de 1MHz (1 tick = 1 microsegundo)
  timerMultiplex = timerBegin(1000000);
  // Asociar la función de interrupción al timer
  timerAttachInterrupt(timerMultiplex, &onTimerMux);
  // Alarma: dispara cada 2500 ticks (= 2500 µs = 2.5 ms), repetitiva (true), sin límite de repeticiones (0)
  timerAlarm(timerMultiplex, 2500, true, 0);

  // Crear la tarea de multiplexado: nombre, stack 2048 bytes, sin parámetros, prioridad 5, sin handle, Core 1
  xTaskCreatePinnedToCore(taskMultiplexado, "MuxTask", 2048, NULL, 5, NULL, 1);

  // Configurar buzzer con LEDC: pin, frecuencia 1kHz, resolución 8 bits (0–255)
  ledcAttach(BUZZER, 1000, 8);
  // Inicializar RGB NeoPixel y apagarlo (estado inicial = negro)
  rgb.begin(); rgb.show();
  // Inicializar bus I2C en pines SDA=8, SCL=9, velocidad 800kHz (fast-mode plus)
  Wire.begin(8, 9); Wire.setClock(800000);
  // Inicializar la OLED con fuente de alimentación interna (SWITCHCAPVCC) y su dirección I2C
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay(); oled.display(); // Limpia pantalla y envía el framebuffer vacío (apaga pixeles)
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP — Máquina de estados principal (se repite indefinidamente)
// ═══════════════════════════════════════════════════════════════════
void loop() {
  unsigned long ahora = millis(); // Captura el tiempo actual una sola vez por iteración

  // Leer estado actual de todos los botones (LOW = presionado por pull-up)
  bool b1 = digitalRead(BTN1), b2 = digitalRead(BTN2);
  bool b3 = digitalRead(BTN3), b4 = digitalRead(BTN4);

  // ── BOTÓN 4 GLOBAL: vuelve al menú principal desde cualquier estado ──
  if (estado != 0 && antB4 && !b4 && (ahora - tB4 > DB)) {
    // antB4=1 y b4=0 → flanco descendente (botón recién presionado), con debounce
    estado = 0; oledNuevo = true;  // Volver al menú y forzar redibujado
    apagarDisplay();               // Limpiar display 7-seg
    digitalWrite(LED1,0); digitalWrite(LED2,0); // Apagar indicadores
    ledcWrite(BUZZER,0);           // Apagar buzzer
    tB4 = ahora;                   // Actualizar timestamp de B4
  }
  antB4 = b4; // Guardar estado actual para detectar flanco en el próximo ciclo

  switch (estado) {

    // ──────────────────────────────────────────────────────────────
    case 0: // MENÚ PRINCIPAL: selección de modo de terapia
    // ──────────────────────────────────────────────────────────────
      if (oledNuevo) {
        drawHeader("SELECCION MODO");          // Encabezado estándar
        oled.setCursor(5, 25); oled.print("B1: ISOTONICO");    // Opción 1
        oled.setCursor(5, 40); oled.print("B2: ESTIRAMIENTO"); // Opción 2 (aún sin handler)
        oled.display();    // Envía el framebuffer a la pantalla física
        oledNuevo = false; // Ya se dibujó, no volver a dibujar hasta nuevo cambio
      }
      // B1 → inicia flujo de isotónico: va a estado 1 con tiempo default 3 minutos
      if (antB1 && !b1 && (ahora-tB1>DB)) {
        estado = 1; cfgTiempo = 180; oledNuevo = true; tB1=ahora;
      }
      // B2 → aquí irá el handler de estiramiento (VER SECCIÓN DE EJEMPLO MÁS ABAJO)
      break;

    // ──────────────────────────────────────────────────────────────
    case 1: // CONFIGURAR DURACIÓN DE SERIE (en segundos, para modo isotónico)
    // ──────────────────────────────────────────────────────────────
      if (oledNuevo) {
        drawHeader("DURACION SERIE");
        oled.setTextSize(2); oled.setCursor(35, 25); // Texto grande centrado
        oled.print(cfgTiempo/60);                     // Minutos
        oled.print(":");
        if(cfgTiempo%60<10) oled.print("0");          // Cero inicial para segundos < 10
        oled.print(cfgTiempo%60);                     // Segundos
        oled.display(); oledNuevo = false;
      }
      // B1 → decrementar 10 segundos (mínimo 60s = 1 minuto)
      if (antB1 && !b1 && (ahora-tB1>DB)) {
        if (cfgTiempo > 60) { cfgTiempo -= 10; oledNuevo = true; } // Resta y redibuja
        else feedback(rgb.Color(255,0,0), 1000); // En el límite: feedback rojo de error
        tB1 = ahora;
      }
      // B2 → incrementar 10 segundos (máximo 300s = 5 minutos)
      if (antB2 && !b2 && (ahora-tB2>DB)) {
        if (cfgTiempo < 300) { cfgTiempo += 10; oledNuevo = true; } // Suma y redibuja
        else feedback(rgb.Color(255,0,0), 1000); // En el límite: feedback rojo de error
        tB2 = ahora;
      }
      // B3 → confirmar y avanzar a configurar repeticiones
      if (antB3 && !b3 && (ahora-tB3>DB)) { estado = 2; cfgReps = 5; oledNuevo = true; tB3=ahora; }
      break;

    // ──────────────────────────────────────────────────────────────
    case 2: // CONFIGURAR NÚMERO DE REPETICIONES
    // ──────────────────────────────────────────────────────────────
      if (oledNuevo) {
        drawHeader("REPETICIONES");
        oled.setTextSize(3); oled.setCursor(50, 25); // Número grande centrado
        oled.print(cfgReps);
        oled.display(); oledNuevo = false;
      }
      // B1 → decrementar reps (mínimo 3)
      if (antB1 && !b1 && (ahora-tB1>DB)) {
        if (cfgReps > 3) { cfgReps--; oledNuevo = true; }
        else feedback(rgb.Color(255,0,0), 1000); // Límite inferior: feedback de error
        tB1 = ahora;
      }
      // B2 → incrementar reps (máximo 12)
      if (antB2 && !b2 && (ahora-tB2>DB)) {
        if (cfgReps < 12) { cfgReps++; oledNuevo = true; }
        else feedback(rgb.Color(255,0,0), 1000); // Límite superior: feedback de error
        tB2 = ahora;
      }
      // B3 → confirmar y avanzar al resumen
      if (antB3 && !b3 && (ahora-tB3>DB)) { estado = 3; oledNuevo = true; tB3=ahora; }
      break;

    // ──────────────────────────────────────────────────────────────
    case 3: // RESUMEN: muestra config final y espera inicio con doble clic en B1
    // ──────────────────────────────────────────────────────────────
      if (oledNuevo) {
        drawHeader("RESUMEN");
        oled.setCursor(5, 20); oled.print("Tiempo: "); oled.print(cfgTiempo/60); oled.print(" min");
        oled.setCursor(5, 32); oled.print("Reps:   "); oled.print(cfgReps);
        oled.setCursor(5, 50); oled.print("2xB1 para iniciar"); // Instrucción de inicio
        oled.display(); oledNuevo = false;
      }
      if (antB1 && !b1 && (ahora - tB1 > DB)) {
        if (ahora - tUltimoClicB1 < 500) { // Segundo clic dentro de 500ms → doble clic detectado
          // Copiar config a variables de sesión activa
          totTiempo = cfgTiempo; totReps = cfgReps; repsHechas = 0;
          tInicio = ahora;      // Marca el inicio de la terapia
          acumPausa = 0;        // Sin tiempo acumulado en pausa al arrancar
          enPausa = false;      // No empieza en pausa
          oledNuevo = true; estado = 4; // Ir a estado de terapia activa
        }
        tUltimoClicB1 = ahora; // Registrar timestamp de este clic (para detectar el siguiente doble clic)
        tB1 = ahora;
      }
      break;

    // ──────────────────────────────────────────────────────────────
    case 4: // TERAPIA ACTIVA (modo isotónico)
    // ──────────────────────────────────────────────────────────────
    {
      bool sw1 = digitalRead(SW1); // SW1=LOW → pausar terapia
      bool sw2 = digitalRead(SW2); // SW2=HIGH → display muestra tiempo; SW2=LOW → display muestra reps

      // Detectar activación de pausa (SW1 baja a LOW mientras no está en pausa)
      if (sw1 == LOW && !enPausa) {
        enPausa = true;           // Entrar en pausa
        tPausa = ahora;           // Registrar cuándo empezó esta pausa
        digitalWrite(LED1, HIGH); // LED1 = indicador visual de pausa
        oledNuevo=true;           // Redibujar para mostrar "PAUSADO"
      }
      // Detectar fin de pausa (SW1 sube a HIGH mientras estaba en pausa)
      if (sw1 == HIGH && enPausa) {
        enPausa = false;                      // Salir de pausa
        acumPausa += (ahora - tPausa);        // Acumular el tiempo que duró esta pausa
        digitalWrite(LED1, LOW);              // Apagar indicador de pausa
        oledNuevo=true;                       // Redibujar para mostrar "EN PROGRESO"
      }

      // LED2 sigue el estado de SW2 (indicador visual de qué muestra el display)
      digitalWrite(LED2, sw2 == LOW ? HIGH : LOW);

      // Calcular tiempo transcurrido excluyendo las pausas
      // Si está en pausa: congelar en el tiempo de inicio de la pausa actual
      // Si no está en pausa: tiempo real - tiempo acumulado en pausas
      unsigned long trans = enPausa ? (tPausa - tInicio - acumPausa)
                                    : (ahora - tInicio - acumPausa);
      int sl = totTiempo - (int)(trans / 1000); // Segundos restantes (ms → s)
      if (sl < 0) sl = 0;                        // Nunca negativo

      // Actualizar el display 7-seg con el valor actual (tiempo o reps según SW2)
      actualizarDisplayFisico(sl, repsHechas, sw2 == HIGH);

      // Doble clic en B1 → registrar una repetición completada
      if (antB1 && !b1 && (ahora - tB1 > DB)) {
        if (ahora - tUltimoClicB1 < 500) { // Doble clic
          repsHechas++;                          // Incrementar contador de reps
          feedback(rgb.Color(0,0,255), 800);    // Feedback azul 800ms = rep registrada
          oledNuevo = true;                      // Redibujar para actualizar contador en OLED
        }
        tUltimoClicB1 = ahora; tB1 = ahora;
      }

      // AVISO FINAL: cuando quedan 15 segundos o menos, parpadear buzzer+RGB amarillo
      if (sl <= 15 && sl > 0 && !enPausa) {
        avisoActivo = true; // Bloquea el apagado automático de buzzer/RGB en el loop
        if (ahora - tAvisoFlash > 375) { // Cambiar estado del flash cada 375ms
          avisoFlashOn = !avisoFlashOn;
          tAvisoFlash = ahora;
          // Alternar buzzer entre 50% y apagado
          ledcWrite(BUZZER, avisoFlashOn ? 128 : 0);
          // Alternar RGB entre amarillo y apagado
          rgb.setPixelColor(0, avisoFlashOn ? rgb.Color(255,200,0) : 0); rgb.show();
        }
      }

      // Redibujar OLED solo si cambió el estado (oledNuevo) o cambió el segundo visible
      if (oledNuevo || sl != lastSlOled) {
        lastSlOled = sl; // Actualizar referencia para detectar cambio de segundo
        drawHeader(enPausa ? "PAUSADO" : "EN PROGRESO");
        oled.setCursor(5, 18); oled.print("Tiempo: ");
        oled.print(sl/60); oled.print(":"); if(sl%60<10) oled.print("0"); oled.print(sl%60);
        oled.setCursor(5, 30); oled.print("Reps:   ");
        oled.print(repsHechas); oled.print("/"); oled.print(totReps);
        oled.display(); oledNuevo = false;
      }

      // Condiciones de fin de terapia (se evalúan en orden de prioridad)
      if (repsHechas >= totReps) terminarTerapia(1); // Reps completadas
      else if (sl <= 0)           terminarTerapia(2); // Tiempo agotado
      else if (antB3 && !b3)      terminarTerapia(3); // Interrumpida por usuario
    }
    break;

    // ──────────────────────────────────────────────────────────────
    case 5: // FIN DE SESIÓN: muestra resultado y espera B4 para salir
    // ──────────────────────────────────────────────────────────────
      if (oledNuevo) {
        drawHeader("FIN SESION");
        // Mostrar razón de fin según el código guardado en razonFin
        oled.setCursor(5, 20);
        oled.print(razonFin==1 ? "COMPLETADO" :
                   razonFin==2 ? "TIEMPO FIN" : "INTERRUMPIDO");
        oled.setCursor(5, 35); oled.print("Reps: "); oled.print(repsHechas); oled.print("/"); oled.print(totReps);
        oled.setCursor(5, 52); oled.print("B4: SALIR"); // B4 global vuelve a estado 0
        oled.display(); oledNuevo = false;
        avisoActivo = false; // Desactivar aviso final al entrar a la pantalla de fin
      }
      break;

  } // fin switch

  // ── APAGADO AUTOMÁTICO DE BUZZER Y RGB ──
  // Solo actúa si NO estamos en el aviso de fin (que maneja su propio encendido/apagado)
  if (!avisoActivo) {
    if (ahora >= tBuzFin) ledcWrite(BUZZER, 0);                  // Apagar buzzer al vencer el tiempo
    if (ahora >= tRgbFin) { rgb.setPixelColor(0, 0); rgb.show(); } // Apagar RGB al vencer el tiempo
  }

  // Guardar estado actual de los botones para detectar flancos en el próximo ciclo
  antB1=b1; antB2=b2; antB3=b3;
  // Nota: antB4 ya se actualiza dentro del bloque global de B4 (al inicio del loop)
}



// ═══════════════════════════════════════════════════════════════════
//  EJEMPLO: HANDLER B2 — MODO ESTIRAMIENTO
//
//  CONCEPTO DEL MODO ESTIRAMIENTO:
//    Cada repetición tiene DOS fases:
//      1. MANTENER: el usuario sostiene la posición (cfgTiempo segundos)
//      2. DESCANSO: el usuario descansa entre reps   (cfgDescanso segundos)
//    Esto se repite cfgReps veces.
//    SW1 NO pausa; en su lugar, el usuario avanza fases con B1 (opcional).
//    B3 interrumpe en cualquier momento.
//
//  FLUJO DE ESTADOS NUEVO:
//    0 ──[B2]──► 6 (cfg tiempo mantener) ──[B3]──► 7 (cfg descanso) ──[B3]──► 8 (cfg reps)
//    ──[B3]──► 9 (resumen estiramiento) ──[2xB1]──► 10 (terapia estiramiento) ──► 5 (fin)
//
//  NOTA: el estado 5 (fin) y las funciones auxiliares ya existen — se reutilizan.
// ═══════════════════════════════════════════════════════════════════

// ─── VARIABLES NUEVAS A AGREGAR (junto a las declaraciones globales existentes) ───

//int cfgDescanso = 15;          // Segundos de descanso entre repeticiones (default 15s)
//int totDescanso = 0;           // Copia activa de cfgDescanso durante la sesión
//bool faseManteniendo = true;   // true = fase MANTENER activa | false = fase DESCANSO activa
//int slFase = 0;                // Segundos restantes en la fase actual (mantener o descanso)
//unsigned long tFaseInicio = 0; // millis() al inicio de la fase actual

// ─── EN ESTADO 0: AGREGAR HANDLER DE B2 ──────────────────────────────────────────

// Dentro del case 0, después del handler de B1, agregar:
/*
  if (antB2 && !b2 && (ahora - tB2 > DB)) {
    estado = 6;          // Ir a configurar tiempo de mantenimiento
    cfgTiempo = 30;      // Default: 30 segundos de mantenimiento por rep
    oledNuevo = true;
    tB2 = ahora;
  }
*/

// ─── ESTADO 6: CONFIGURAR TIEMPO DE MANTENIMIENTO ────────────────────────────────
/*
  case 6:
    if (oledNuevo) {
      drawHeader("TIEMPO MANTENER");
      oled.setTextSize(2); oled.setCursor(40, 25);
      oled.print(cfgTiempo);       // Muestra segundos (no necesita M:SS, rango pequeño)
      oled.print("s");
      oled.display(); oledNuevo = false;
    }
    // B1 → restar 5 segundos (mínimo 10s)
    if (antB1 && !b1 && (ahora - tB1 > DB)) {
      if (cfgTiempo > 10) { cfgTiempo -= 5; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB1 = ahora;
    }
    // B2 → sumar 5 segundos (máximo 120s = 2 minutos)
    if (antB2 && !b2 && (ahora - tB2 > DB)) {
      if (cfgTiempo < 120) { cfgTiempo += 5; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB2 = ahora;
    }
    // B3 → confirmar y avanzar a configurar descanso
    if (antB3 && !b3 && (ahora - tB3 > DB)) {
      estado = 7; cfgDescanso = 15; oledNuevo = true; tB3 = ahora;
    }
    break;
*/

// ─── ESTADO 7: CONFIGURAR TIEMPO DE DESCANSO ─────────────────────────────────────
/*
  case 7:
    if (oledNuevo) {
      drawHeader("TIEMPO DESCANSO");
      oled.setTextSize(2); oled.setCursor(40, 25);
      oled.print(cfgDescanso);
      oled.print("s");
      oled.display(); oledNuevo = false;
    }
    // B1 → restar 5 segundos (mínimo 5s)
    if (antB1 && !b1 && (ahora - tB1 > DB)) {
      if (cfgDescanso > 5) { cfgDescanso -= 5; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB1 = ahora;
    }
    // B2 → sumar 5 segundos (máximo 60s)
    if (antB2 && !b2 && (ahora - tB2 > DB)) {
      if (cfgDescanso < 60) { cfgDescanso += 5; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB2 = ahora;
    }
    // B3 → confirmar y avanzar a configurar reps
    if (antB3 && !b3 && (ahora - tB3 > DB)) {
      estado = 8; cfgReps = 5; oledNuevo = true; tB3 = ahora;
    }
    break;
*/

// ─── ESTADO 8: CONFIGURAR REPETICIONES (igual al estado 2) ───────────────────────
// Se puede REUTILIZAR el estado 2 si la siguiente transición distingue por modo,
// o duplicar la lógica apuntando al estado 9 en vez del 3.
/*
  case 8:
    if (oledNuevo) {
      drawHeader("REPETICIONES");
      oled.setTextSize(3); oled.setCursor(50, 25);
      oled.print(cfgReps);
      oled.display(); oledNuevo = false;
    }
    if (antB1 && !b1 && (ahora-tB1>DB)) {
      if (cfgReps > 3) { cfgReps--; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB1 = ahora;
    }
    if (antB2 && !b2 && (ahora-tB2>DB)) {
      if (cfgReps < 12) { cfgReps++; oledNuevo = true; }
      else feedback(rgb.Color(255,0,0), 800);
      tB2 = ahora;
    }
    if (antB3 && !b3 && (ahora-tB3>DB)) {
      estado = 9; oledNuevo = true; tB3 = ahora;
    }
    break;
*/

// ─── ESTADO 9: RESUMEN ESTIRAMIENTO ──────────────────────────────────────────────
/*
  case 9:
    if (oledNuevo) {
      drawHeader("ESTIRAMIENTO");
      oled.setCursor(5, 18); oled.print("Mantener: "); oled.print(cfgTiempo);   oled.print("s");
      oled.setCursor(5, 28); oled.print("Descanso: "); oled.print(cfgDescanso); oled.print("s");
      oled.setCursor(5, 38); oled.print("Reps:     "); oled.print(cfgReps);
      oled.setCursor(5, 52); oled.print("2xB1 para iniciar");
      oled.display(); oledNuevo = false;
    }
    if (antB1 && !b1 && (ahora - tB1 > DB)) {
      if (ahora - tUltimoClicB1 < 500) {
        // Copiar config a variables activas de la sesión
        totTiempo   = cfgTiempo;    // Tiempo de mantenimiento por rep
        totDescanso = cfgDescanso;  // Tiempo de descanso entre reps
        totReps     = cfgReps;
        repsHechas  = 0;
        faseManteniendo = true;     // Empezar siempre en fase de mantener
        tFaseInicio = ahora;        // Iniciar temporizador de la primera fase
        estado = 10; oledNuevo = true;
      }
      tUltimoClicB1 = ahora; tB1 = ahora;
    }
    break;
*/

// ─── ESTADO 10: TERAPIA ESTIRAMIENTO ─────────────────────────────────────────────
/*
  case 10:
  {
    // Calcular segundos restantes en la fase actual (sin pausas, el estiramiento no pausa)
    int slFase = (faseManteniendo ? totTiempo : totDescanso)
                  - (int)((ahora - tFaseInicio) / 1000);
    if (slFase < 0) slFase = 0;

    // ── TRANSICIÓN DE FASE ──
    if (slFase <= 0) {
      if (faseManteniendo) {
        // Fin de fase MANTENER → iniciar DESCANSO (salvo en la última rep)
        if (repsHechas + 1 >= totReps) {
          // Era la última rep: terminar
          repsHechas++;
          terminarTerapia(1); // Completado
          break;
        }
        faseManteniendo = false;       // Cambiar a fase descanso
        tFaseInicio = ahora;           // Reiniciar temporizador de fase
        feedback(rgb.Color(0,255,0), 600); // Feedback verde: completó una rep
        oledNuevo = true;
      } else {
        // Fin de DESCANSO → iniciar siguiente MANTENER
        repsHechas++;                  // Contar la rep que acaba de terminar su descanso
        faseManteniendo = true;        // Volver a fase de mantener
        tFaseInicio = ahora;           // Reiniciar temporizador de fase
        feedback(rgb.Color(0,0,255), 400); // Feedback azul: empieza nueva rep
        oledNuevo = true;
      }
    }

    // ── DISPLAY 7-SEG: tiempo restante de la fase actual ──
    // SW2=HIGH → mostrar tiempo de fase en display; SW2=LOW → mostrar repsHechas
    bool sw2 = digitalRead(SW2);
    digitalWrite(LED2, sw2 == LOW ? HIGH : LOW);
    actualizarDisplayFisico(slFase, repsHechas, sw2 == HIGH);

    // ── AVISO FINAL DE FASE (parpadeo en últimos 5 segundos de cada fase) ──
    if (slFase <= 5 && slFase > 0) {
      avisoActivo = true;
      if (ahora - tAvisoFlash > 375) {
        avisoFlashOn = !avisoFlashOn;
        tAvisoFlash = ahora;
        // Color diferente según la fase: amarillo en mantener, cian en descanso
        uint32_t colorAviso = faseManteniendo ? rgb.Color(255,200,0) : rgb.Color(0,200,200);
        ledcWrite(BUZZER, avisoFlashOn ? 64 : 0);   // Volumen más bajo en el aviso de estiramiento
        rgb.setPixelColor(0, avisoFlashOn ? colorAviso : 0); rgb.show();
      }
    } else {
      avisoActivo = false; // Fuera del rango de aviso, dejar que se apague solo
    }

    // ── OLED: actualizar si cambió la fase o el segundo ──
    if (oledNuevo || slFase != lastSlOled) {
      lastSlOled = slFase;
      drawHeader(faseManteniendo ? "MANTENER" : "DESCANSO");
      oled.setCursor(5, 18); oled.print("Tiempo: ");
      oled.print(slFase); oled.print("s");
      oled.setCursor(5, 30); oled.print("Rep: ");
      oled.print(repsHechas + 1); oled.print("/"); oled.print(totReps);
      // Mostrar barra visual de progreso de la fase (ancho proporcional)
      int durFase = faseManteniendo ? totTiempo : totDescanso;
      int barW = (durFase > 0) ? (slFase * 118) / durFase : 0; // 118 px de ancho útil
      oled.drawRect(5, 44, 118, 8, SSD1306_WHITE);              // Marco de la barra
      oled.fillRect(5, 44, barW, 8, SSD1306_WHITE);             // Relleno proporcional
      oled.display(); oledNuevo = false;
    }

    // B3 → interrumpir la sesión
    if (antB3 && !b3) terminarTerapia(3);
  }
  break;
*/
