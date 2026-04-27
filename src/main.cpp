#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <ESP32Servo.h>

BluetoothSerial SerialBT;
hd44780_I2Cexp lcd;
Servo servoBrazoIzq;
Servo servoBrazoDer;
Servo servoCabeza;

// ─── Enumeraciones ──────────────────────────────────────────

enum class DisplayMode : uint8_t {
  Standby,
  Driving,
  Mode1Input,
  Mode2Telemetry
};

enum class TelemetryFocus : uint8_t {
  None,
  Hour,
  Weather,
  Name
};

// Movimientos especiales (no bloqueantes)
enum class SpecialMove : uint8_t {
  None,
  Explore,   // X - movimiento aleatorio
  Dance,     // D - secuencia de baile
  Scan,      // V - escaneo 360 grados
  Prank      // P - susto (avance rapido + ojos)
};

struct DebouncedButton {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChangeAt;
};

// Paso individual de una secuencia de movimiento especial
struct MoveStep {
  void (*action)();        // funcion motora a ejecutar
  unsigned long durationMs; // duracion antes de avanzar al siguiente paso
};

// Estado de ejecucion de un movimiento especial
struct MoveState {
  SpecialMove activeMove = SpecialMove::None;
  const MoveStep* steps = nullptr;
  uint8_t totalSteps = 0;
  uint8_t currentStep = 0;
  unsigned long stepStartedAt = 0;
};

// ─── Pines ──────────────────────────────────────────────────

constexpr int ledPin = 5;
constexpr int lcdColumns = 20;
constexpr int lcdRows = 4;
constexpr int i2cSdaPin = 21;
constexpr int i2cSclPin = 22;

constexpr int hourButtonPin = 25;
constexpr int weatherButtonPin = 26;
constexpr int nameButtonPin = 27;

// L298N
constexpr int pinIN1 = 16;
constexpr int pinIN2 = 17;
constexpr int pinIN3 = 18;
constexpr int pinIN4 = 19;

// Servos — ajustar segun cableado real
constexpr int servoBrazoIzqPin = 13;
constexpr int servoBrazoDerPin = 23;
constexpr int servoCabezaPin   = 32;

// ─── Constantes ─────────────────────────────────────────────

// Angulos de servos — ajustar segun montaje fisico
constexpr int brazoIzqReposo  =   0;
constexpr int brazoIzqActivo  =  90;
constexpr int brazoDerReposo  = 180;
constexpr int brazoDerActivo  =  90;
constexpr int cabezaIzqGrados =  45;
constexpr int cabezaCenGrados =  90;
constexpr int cabezaDerGrados = 135;

constexpr unsigned long scrollIntervalMs = 220;
constexpr unsigned long commandIdleTimeoutMs = 120;
constexpr unsigned long buttonDebounceMs = 35;
constexpr unsigned long greetingRepeatGuardMs = 1500;
constexpr uint8_t wallECharIndex = 0;
constexpr char bluetoothName[] = "Car-ESP32";

// ─── Iconos LCD ─────────────────────────────────────────────

uint8_t wallEIcon[8] = {
  B00000,
  B01010,
  B11111,
  B01110,
  B11111,
  B10101,
  B01110,
  B00000
};

// ─── Estado global ──────────────────────────────────────────

DebouncedButton hourButton{hourButtonPin, HIGH, HIGH, 0};
DebouncedButton weatherButton{weatherButtonPin, HIGH, HIGH, 0};
DebouncedButton nameButton{nameButtonPin, HIGH, HIGH, 0};

String inputBuffer;
String scrollMessage = " ";
String telemetryHour = "--:--";
String telemetryWeather = "Sin dato";
String robotName = "Wall-E";
String greetingMessage = " ";

DisplayMode currentMode = DisplayMode::Standby;
TelemetryFocus currentTelemetryFocus = TelemetryFocus::None;
MoveState moveState;

bool brazosActivos = false;

unsigned long lastByteReceivedAt = 0;
unsigned long lastScrollAt = 0;
unsigned long lastGreetingSentAt = 0;
int scrollOffset = 0;
bool lcdReady = false;
String lastGreetingPayload;

// ─── Prototipos ─────────────────────────────────────────────

void sendBluetoothLine(const String& message);
void moverAdelante();
void moverAtras();
void girarIzquierda();
void girarDerecha();
void detenerMotores();
void cancelSpecialMove();
void moverBrazos();
void cabezaIzquierda();
void cabezaCentro();
void cabezaDerecha();

// ═══════════════════════════════════════════════════════════
// LCD helpers (sin cambios respecto al original)
// ═══════════════════════════════════════════════════════════

String makeSpaces(size_t count) {
  String spaces;
  spaces.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    spaces += ' ';
  }
  return spaces;
}

String fitLine(const String& value) {
  String result = value.substring(0, lcdColumns);
  while (result.length() < lcdColumns) {
    result += ' ';
  }
  return result;
}

char mapUtf8Pair(uint8_t firstByte, uint8_t secondByte) {
  if (firstByte == 0xC3) {
    switch (secondByte) {
      case 0x81: return 'A';
      case 0x89: return 'E';
      case 0x8D: return 'I';
      case 0x91: return 'N';
      case 0x93: return 'O';
      case 0x9A: return 'U';
      case 0x9C: return 'U';
      case 0xA1: return 'a';
      case 0xA9: return 'e';
      case 0xAD: return 'i';
      case 0xB1: return 'n';
      case 0xB3: return 'o';
      case 0xBA: return 'u';
      case 0xBC: return 'u';
      default: return '?';
    }
  }
  if (firstByte == 0xC2) {
    switch (secondByte) {
      case 0xA1: return '!';
      case 0xBF: return '?';
      default: return '?';
    }
  }
  return '?';
}

String sanitizeForLcd(const String& value) {
  String sanitized;
  sanitized.reserve(value.length());

  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t current = static_cast<uint8_t>(value[i]);

    if (current >= 32 && current <= 126) {
      sanitized += static_cast<char>(current);
      continue;
    }
    if (current == '\t' || current == '\n' || current == '\r') {
      sanitized += ' ';
      continue;
    }
    if (i + 1 < value.length()) {
      const uint8_t next = static_cast<uint8_t>(value[i + 1]);
      sanitized += mapUtf8Pair(current, next);
      ++i;
      continue;
    }
    sanitized += '?';
  }

  sanitized.trim();
  if (sanitized.length() == 0) sanitized = " ";
  if (sanitized.length() > 200) sanitized = sanitized.substring(0, 200);
  return sanitized;
}

void writeLine(int row, const String& text) {
  if (!lcdReady) return;
  lcd.setCursor(0, row);
  lcd.print(fitLine(text));
}

void clearScreen() {
  if (!lcdReady) return;
  for (int row = 0; row < lcdRows; ++row) {
    writeLine(row, "");
  }
}

void writeCenteredLine(int row, const String& text) {
  const String clipped = text.substring(0, lcdColumns);
  const int leftPadding = max(0, (lcdColumns - static_cast<int>(clipped.length())) / 2);
  writeLine(row, makeSpaces(leftPadding) + clipped);
}

void drawCenteredMessage(const String& message) {
  clearScreen();
  const String clean = sanitizeForLcd(message);
  if (clean.length() <= lcdColumns) {
    writeCenteredLine(1, clean);
    return;
  }
  int splitAt = clean.lastIndexOf(' ', lcdColumns);
  if (splitAt <= 0) splitAt = lcdColumns;
  String firstLine = clean.substring(0, splitAt);
  String secondLine = clean.substring(splitAt);
  firstLine.trim();
  secondLine.trim();
  writeCenteredLine(1, firstLine);
  writeCenteredLine(2, secondLine);
}

// ═══════════════════════════════════════════════════════════
// Pantallas LCD por modo
// ═══════════════════════════════════════════════════════════

void drawStandbyScreen() {
  if (!lcdReady) return;
  clearScreen();
  writeCenteredLine(1, "Paga 0.50 ctvs");
  writeCenteredLine(2, "por un saludo");
}

void drawDrivingScreen() {
  if (!lcdReady) return;
  clearScreen();
  writeCenteredLine(1, "Modo Control");
  writeCenteredLine(2, "Listo...");
}

void drawTelemetryScreen() {
  drawCenteredMessage("Presiona un boton");
}

// Muestra el nombre del movimiento especial en el LCD
void drawSpecialMoveScreen(const String& moveName) {
  if (!lcdReady) return;
  clearScreen();
  writeCenteredLine(0, ">>> ESPECIAL <<<");
  writeCenteredLine(2, moveName);
}

// ═══════════════════════════════════════════════════════════
// Mode1 scroll (sin cambios)
// ═══════════════════════════════════════════════════════════

String buildMode1Frame() {
  const String track = makeSpaces(lcdColumns) + scrollMessage + makeSpaces(lcdColumns);
  if (scrollOffset > track.length() - lcdColumns) {
    scrollOffset = 0;
  }
  return track.substring(scrollOffset, scrollOffset + lcdColumns);
}

void refreshMode1Screen() {
  if (!lcdReady || currentMode != DisplayMode::Mode1Input) return;
  const String frame = buildMode1Frame();
  for (int row = 0; row < lcdRows; ++row) {
    writeLine(row, frame);
  }
}

// ═══════════════════════════════════════════════════════════
// Gestion de modos
// ═══════════════════════════════════════════════════════════

void setMode(DisplayMode newMode) {
  currentMode = newMode;
  scrollOffset = 0;
  lastScrollAt = 0;
  if (currentMode != DisplayMode::Mode2Telemetry) {
    currentTelemetryFocus = TelemetryFocus::None;
  }

  if (!lcdReady) return;

  switch (currentMode) {
    case DisplayMode::Standby:
      drawStandbyScreen();
      break;
    case DisplayMode::Driving:
      drawDrivingScreen();
      break;
    case DisplayMode::Mode1Input:
      clearScreen();
      refreshMode1Screen();
      break;
    case DisplayMode::Mode2Telemetry:
      currentTelemetryFocus = TelemetryFocus::None;
      drawTelemetryScreen();
      break;
  }
}

void setScrollMessage(const String& newMessage) {
  scrollMessage = sanitizeForLcd(newMessage);
  scrollOffset = 0;
  lastScrollAt = 0;
  if (currentMode == DisplayMode::Mode1Input) {
    refreshMode1Screen();
  }
}

void setGreetingMessage(const String& input) {
  String cleanedInput = input;
  cleanedInput.trim();
  if (cleanedInput.length() == 0) return;

  greetingMessage = sanitizeForLcd(cleanedInput);
  setScrollMessage(greetingMessage);

  const String greetingPayload = "TTS:" + greetingMessage;
  if (greetingPayload == lastGreetingPayload && millis() - lastGreetingSentAt < greetingRepeatGuardMs) {
    return;
  }
  lastGreetingPayload = greetingPayload;
  lastGreetingSentAt = millis();
  sendBluetoothLine(greetingPayload);
}

void renderTelemetryFocus() {
  if (currentMode != DisplayMode::Mode2Telemetry) return;

  switch (currentTelemetryFocus) {
    case TelemetryFocus::Hour:    drawCenteredMessage(telemetryHour);    return;
    case TelemetryFocus::Weather: drawCenteredMessage(telemetryWeather); return;
    case TelemetryFocus::Name:    drawCenteredMessage(robotName);        return;
    default:                      drawTelemetryScreen();                 return;
  }
}

void setTelemetryValue(TelemetryFocus field, const String& newValue) {
  const String sanitizedValue = sanitizeForLcd(newValue);
  if (field == TelemetryFocus::Hour)    telemetryHour    = sanitizedValue;
  if (field == TelemetryFocus::Weather) telemetryWeather = sanitizedValue;
  if (field == TelemetryFocus::Name)    robotName        = sanitizedValue;

  if (currentMode == DisplayMode::Mode2Telemetry && currentTelemetryFocus == field) {
    renderTelemetryFocus();
  }
}

// ═══════════════════════════════════════════════════════════
// Funciones motoras basicas
// ═══════════════════════════════════════════════════════════

void moverAdelante() {
  digitalWrite(pinIN1, HIGH);
  digitalWrite(pinIN2, LOW);
  digitalWrite(pinIN3, HIGH);
  digitalWrite(pinIN4, LOW);
}

void moverAtras() {
  digitalWrite(pinIN1, LOW);
  digitalWrite(pinIN2, HIGH);
  digitalWrite(pinIN3, LOW);
  digitalWrite(pinIN4, HIGH);
}

void girarIzquierda() {
  digitalWrite(pinIN1, HIGH);
  digitalWrite(pinIN2, LOW);
  digitalWrite(pinIN3, LOW);
  digitalWrite(pinIN4, HIGH);
}

void girarDerecha() {
  digitalWrite(pinIN1, LOW);
  digitalWrite(pinIN2, HIGH);
  digitalWrite(pinIN3, HIGH);
  digitalWrite(pinIN4, LOW);
}

void detenerMotores() {
  digitalWrite(pinIN1, LOW);
  digitalWrite(pinIN2, LOW);
  digitalWrite(pinIN3, LOW);
  digitalWrite(pinIN4, LOW);
}

void encenderOjos() {
  digitalWrite(ledPin, HIGH);
}

void apagarOjos() {
  digitalWrite(ledPin, LOW);
}

// ═══════════════════════════════════════════════════════════
// Funciones de servomotores
// ═══════════════════════════════════════════════════════════

void moverBrazos() {
  brazosActivos = !brazosActivos;
  servoBrazoIzq.write(brazosActivos ? brazoIzqActivo : brazoIzqReposo);
  servoBrazoDer.write(brazosActivos ? brazoDerActivo : brazoDerReposo);
}

void cabezaIzquierda() { servoCabeza.write(cabezaIzqGrados); }
void cabezaCentro()    { servoCabeza.write(cabezaCenGrados); }
void cabezaDerecha()   { servoCabeza.write(cabezaDerGrados); }

// ═══════════════════════════════════════════════════════════
// Secuencias de movimientos especiales (tablas de pasos)
// ═══════════════════════════════════════════════════════════

// ── Bailar (Dance) ──────────────────────────────────────────
// Coreografia: avances cortos alternados con giros, ida y vuelta.
static const MoveStep danceSteps[] = {
  { moverAdelante,    300 },
  { girarIzquierda,   350 },
  { moverAdelante,    250 },
  { girarDerecha,     350 },
  { moverAtras,       300 },
  { girarDerecha,     400 },
  { moverAdelante,    200 },
  { girarIzquierda,   500 },   // giro largo
  { moverAtras,       250 },
  { girarDerecha,     500 },   // giro largo
  { moverAdelante,    300 },
  { girarIzquierda,   300 },
  { detenerMotores,     0 },   // fin
};
constexpr uint8_t danceStepCount = sizeof(danceSteps) / sizeof(danceSteps[0]);

// ── Vigilar (Scan 360) ─────────────────────────────────────
// 4 giros de ~90 grados con pausas de observacion.
static const MoveStep scanSteps[] = {
  { girarIzquierda,   400 },   // ~90 grados
  { detenerMotores,   800 },   // observar
  { girarIzquierda,   400 },
  { detenerMotores,   800 },
  { girarIzquierda,   400 },
  { detenerMotores,   800 },
  { girarIzquierda,   400 },
  { detenerMotores,   500 },
  { detenerMotores,     0 },   // fin
};
constexpr uint8_t scanStepCount = sizeof(scanSteps) / sizeof(scanSteps[0]);

// ── Susto (Prank) ──────────────────────────────────────────
// Ojos encendidos, avance rapido, frenado brusco, pausa dramatica.
static const MoveStep prankSteps[] = {
  { encenderOjos,     100 },   // ojos ON
  { moverAdelante,    900 },   // carga hacia adelante
  { detenerMotores,   150 },   // frenazo
  { moverAtras,       150 },   // retroceso sutil
  { detenerMotores,   600 },   // pausa dramatica
  { apagarOjos,       200 },   // ojos OFF
  { detenerMotores,     0 },   // fin
};
constexpr uint8_t prankStepCount = sizeof(prankSteps) / sizeof(prankSteps[0]);

// ── Explorar (Explore) ─────────────────────────────────────
// Se genera dinamicamente con pasos aleatorios.
// Usamos un buffer estatico con capacidad maxima.
constexpr uint8_t exploreMaxSteps = 16;
static MoveStep exploreSteps[exploreMaxSteps];
static uint8_t exploreStepCount = 0;

void generateExploreSequence() {
  uint8_t idx = 0;
  int segments = random(4, 7); // 4-6 segmentos de movimiento

  for (int seg = 0; seg < segments && idx < exploreMaxSteps - 1; ++seg) {
    // Avanzar un tiempo aleatorio
    exploreSteps[idx++] = { moverAdelante, (unsigned long)random(500, 1500) };

    // Girar aleatoriamente
    if (idx < exploreMaxSteps - 1) {
      auto turnFn = random(0, 2) == 0 ? girarIzquierda : girarDerecha;
      exploreSteps[idx++] = { turnFn, (unsigned long)random(200, 600) };
    }
  }

  // Paso final: detener
  exploreSteps[idx++] = { detenerMotores, 0 };
  exploreStepCount = idx;
}

// ═══════════════════════════════════════════════════════════
// Maquina de estados para movimientos especiales
// ═══════════════════════════════════════════════════════════

void startSpecialMove(SpecialMove move) {
  // Cancelar movimiento previo si existe
  cancelSpecialMove();

  moveState.activeMove = move;
  moveState.currentStep = 0;
  moveState.stepStartedAt = millis();

  switch (move) {
    case SpecialMove::Dance:
      moveState.steps = danceSteps;
      moveState.totalSteps = danceStepCount;
      drawSpecialMoveScreen("Bailando!");
      sendBluetoothLine("MOVE:DANCE");
      break;

    case SpecialMove::Scan:
      moveState.steps = scanSteps;
      moveState.totalSteps = scanStepCount;
      drawSpecialMoveScreen("Vigilando...");
      sendBluetoothLine("MOVE:SCAN");
      break;

    case SpecialMove::Prank:
      moveState.steps = prankSteps;
      moveState.totalSteps = prankStepCount;
      drawSpecialMoveScreen("SUSTO!");
      sendBluetoothLine("MOVE:PRANK");
      break;

    case SpecialMove::Explore:
      generateExploreSequence();
      moveState.steps = exploreSteps;
      moveState.totalSteps = exploreStepCount;
      drawSpecialMoveScreen("Explorando...");
      sendBluetoothLine("MOVE:EXPLORE");
      break;

    default:
      moveState.activeMove = SpecialMove::None;
      return;
  }

  // Ejecutar el primer paso inmediatamente
  if (moveState.totalSteps > 0) {
    moveState.steps[0].action();
  }

  Serial.print("Movimiento especial iniciado: ");
  Serial.println(static_cast<int>(move));
}

void cancelSpecialMove() {
  if (moveState.activeMove == SpecialMove::None) return;

  detenerMotores();

  // Si el prank encendio los ojos, apagarlos
  if (moveState.activeMove == SpecialMove::Prank) {
    apagarOjos();
  }

  moveState.activeMove = SpecialMove::None;
  moveState.steps = nullptr;
  moveState.totalSteps = 0;
  moveState.currentStep = 0;

  sendBluetoothLine("MOVE:DONE");

  // Restaurar pantalla LCD segun modo actual
  setMode(currentMode);
}

// Llamado desde loop(): avanza la secuencia si el timer del paso actual expiro.
void updateSpecialMove() {
  if (moveState.activeMove == SpecialMove::None) return;
  if (moveState.steps == nullptr) return;

  const MoveStep& step = moveState.steps[moveState.currentStep];

  // Si la duracion es 0, es el paso final (fin de secuencia)
  if (step.durationMs == 0) {
    cancelSpecialMove();
    return;
  }

  if (millis() - moveState.stepStartedAt >= step.durationMs) {
    // Avanzar al siguiente paso
    moveState.currentStep++;

    if (moveState.currentStep >= moveState.totalSteps) {
      cancelSpecialMove();
      return;
    }

    moveState.stepStartedAt = millis();
    moveState.steps[moveState.currentStep].action();
  }
}

bool isSpecialMoveActive() {
  return moveState.activeMove != SpecialMove::None;
}

// ═══════════════════════════════════════════════════════════
// Infraestructura BT / Botones / LCD
// ═══════════════════════════════════════════════════════════

bool initLcd() {
  Wire.begin(i2cSdaPin, i2cSclPin);
  const int status = lcd.begin(lcdColumns, lcdRows);
  if (status != 0) {
    Serial.print("No se pudo inicializar el LCD. Codigo: ");
    Serial.println(status);
    return false;
  }
  lcd.clear();
  lcd.backlight();
  lcd.createChar(wallECharIndex, wallEIcon);
  return true;
}

void sendBluetoothLine(const String& message) {
  SerialBT.println(message);
  Serial.println(message);
}

bool wasButtonPressed(DebouncedButton& button) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChangeAt = millis();
  }
  if (millis() - button.lastChangeAt < buttonDebounceMs) return false;
  if (button.stableState != button.lastReading) {
    button.stableState = button.lastReading;
    if (button.stableState == LOW) return true;
  }
  return false;
}

void processTelemetryButtons() {
  if (currentMode != DisplayMode::Mode2Telemetry) return;

  if (wasButtonPressed(hourButton)) {
    currentTelemetryFocus = TelemetryFocus::Hour;
    renderTelemetryFocus();
  }
  if (wasButtonPressed(weatherButton)) {
    currentTelemetryFocus = TelemetryFocus::Weather;
    renderTelemetryFocus();
  }
  if (wasButtonPressed(nameButton)) {
    currentTelemetryFocus = TelemetryFocus::Name;
    renderTelemetryFocus();
  }
}

// ═══════════════════════════════════════════════════════════
// Procesamiento de comandos Bluetooth
// ═══════════════════════════════════════════════════════════

void handleCommand(String rawCommand) {
  rawCommand.trim();
  if (rawCommand.length() == 0) return;

  String upperCommand = rawCommand;
  upperCommand.toUpperCase();

  // ── Movimiento basico ─────────────────────────────────────
  // Cualquier comando de movimiento manual cancela un movimiento especial.
  if (upperCommand == "F") { cancelSpecialMove(); moverAdelante();   return; }
  if (upperCommand == "B") { cancelSpecialMove(); moverAtras();      return; }
  if (upperCommand == "L") { cancelSpecialMove(); girarIzquierda();  return; }
  if (upperCommand == "R") { cancelSpecialMove(); girarDerecha();    return; }
  if (upperCommand == "S") { cancelSpecialMove(); detenerMotores();  return; }
  if (upperCommand == "G") { cancelSpecialMove(); girarIzquierda(); delay(400); detenerMotores(); return; }
  if (upperCommand == "H") { cancelSpecialMove(); girarDerecha();   delay(400); detenerMotores(); return; }

  // ── Movimientos especiales (novedosos) ────────────────────
  if (upperCommand == "X") { startSpecialMove(SpecialMove::Explore); return; }
  if (upperCommand == "D") { startSpecialMove(SpecialMove::Dance);   return; }
  if (upperCommand == "V") { startSpecialMove(SpecialMove::Scan);    return; }
  if (upperCommand == "P") { startSpecialMove(SpecialMove::Prank);   return; }

  // ── Modos de pantalla ─────────────────────────────────────
  if (upperCommand == "STANDBY") { setMode(DisplayMode::Standby);        return; }
  if (upperCommand == "DRIVING")  { setMode(DisplayMode::Driving);        return; }
  if (upperCommand == "MODE1")    { setMode(DisplayMode::Mode1Input);     return; }
  if (upperCommand == "MODE2")    { setMode(DisplayMode::Mode2Telemetry); return; }

  // ── Control de ojos (LED) ─────────────────────────────────
  if (upperCommand == "ON") {
    encenderOjos();
    sendBluetoothLine("OK:ON");
    return;
  }
  if (upperCommand == "OFF") {
    apagarOjos();
    sendBluetoothLine("OK:OFF");
    return;
  }

  // ── Limpieza ──────────────────────────────────────────────
  if (upperCommand == "CLEAR") {
    setScrollMessage(" ");
    return;
  }

  // ── Servos ────────────────────────────────────────────────
  if (upperCommand == "BRAZOS")     { moverBrazos();     return; }
  if (upperCommand == "CABEZA_IZQ") { cabezaIzquierda(); return; }
  if (upperCommand == "CABEZA_CEN") { cabezaCentro();    return; }
  if (upperCommand == "CABEZA_DER") { cabezaDerecha();   return; }

  // ── Mensajes / Saludos ────────────────────────────────────
  if (upperCommand.startsWith("SALUDO:"))   { setGreetingMessage(rawCommand.substring(7));  return; }
  if (upperCommand.startsWith("GREETING:")) { setGreetingMessage(rawCommand.substring(9));  return; }
  if (upperCommand.startsWith("TEXT:"))     { setGreetingMessage(rawCommand.substring(5));  return; }
  if (upperCommand.startsWith("MSG:"))      { setGreetingMessage(rawCommand.substring(4));  return; }

  // ── Telemetria ────────────────────────────────────────────
  if (upperCommand.startsWith("SET_HORA:"))    { setTelemetryValue(TelemetryFocus::Hour,    rawCommand.substring(9));  return; }
  if (upperCommand.startsWith("SET_TIEMPO:"))  { setTelemetryValue(TelemetryFocus::Weather, rawCommand.substring(11)); return; }
  if (upperCommand.startsWith("SET_NOMBRE:"))  { setTelemetryValue(TelemetryFocus::Name,    rawCommand.substring(11)); return; }
  if (upperCommand.startsWith("HORA:"))        { setTelemetryValue(TelemetryFocus::Hour,    rawCommand.substring(5));  return; }
  if (upperCommand.startsWith("TIEMPO:"))      { setTelemetryValue(TelemetryFocus::Weather, rawCommand.substring(7));  return; }
  if (upperCommand.startsWith("NOMBRE:"))      { setTelemetryValue(TelemetryFocus::Name,    rawCommand.substring(7));  return; }

  Serial.print("Comando ignorado: ");
  Serial.println(rawCommand);
}

void readBluetoothCommands() {
  while (SerialBT.available()) {
    const char incoming = static_cast<char>(SerialBT.read());
    lastByteReceivedAt = millis();

    if (incoming == '\r') continue;

    if (incoming == '\n') {
      handleCommand(inputBuffer);
      inputBuffer = "";
      continue;
    }

    if (static_cast<uint8_t>(incoming) >= 32) {
      inputBuffer += incoming;
    }

    // Fast-path: caracteres individuales que no son prefijo de ningun comando largo.
    // F, B, L, R  → direccion (ya existentes)
    // X           → explorar  (ningún comando empieza con X)
    // V           → vigilar   (ningún comando empieza con V)
    // P           → susto     (ningún comando empieza con P)
    // NOTA: D NO esta en fast-path porque colisiona con DRIVING.
    //       S NO esta en fast-path porque colisiona con STANDBY/SET_*.
    //       G NO esta en fast-path porque colisiona con GREETING:.
    if (inputBuffer.length() == 1) {
      char c = inputBuffer[0];
      if (c == 'F' || c == 'B' || c == 'L' || c == 'R' ||
          c == 'f' || c == 'b' || c == 'l' || c == 'r' ||
          c == 'X' || c == 'x' ||
          c == 'V' || c == 'v' ||
          c == 'P' || c == 'p') {
        handleCommand(inputBuffer);
        inputBuffer = "";
        continue;
      }
    }

    if (inputBuffer.length() > 260) {
      inputBuffer = inputBuffer.substring(0, 260);
    }
  }

  if (inputBuffer.length() > 0 && millis() - lastByteReceivedAt >= commandIdleTimeoutMs) {
    handleCommand(inputBuffer);
    inputBuffer = "";
  }
}

// ═══════════════════════════════════════════════════════════
// Mode1 scroll
// ═══════════════════════════════════════════════════════════

void updateMode1Scroll() {
  if (currentMode != DisplayMode::Mode1Input || !lcdReady) return;
  if (millis() - lastScrollAt < scrollIntervalMs) return;

  lastScrollAt = millis();
  refreshMode1Screen();
  ++scrollOffset;

  const int maxOffset = scrollMessage.length() + lcdColumns;
  if (scrollOffset > maxOffset) scrollOffset = 0;
}

// ═══════════════════════════════════════════════════════════
// Setup & Loop
// ═══════════════════════════════════════════════════════════

void setupButtons() {
  pinMode(hourButton.pin, INPUT_PULLUP);
  pinMode(weatherButton.pin, INPUT_PULLUP);
  pinMode(nameButton.pin, INPUT_PULLUP);
}

void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  pinMode(pinIN1, OUTPUT);
  pinMode(pinIN2, OUTPUT);
  pinMode(pinIN3, OUTPUT);
  pinMode(pinIN4, OUTPUT);
  detenerMotores();

  servoBrazoIzq.attach(servoBrazoIzqPin);
  servoBrazoDer.attach(servoBrazoDerPin);
  servoCabeza.attach(servoCabezaPin);
  servoBrazoIzq.write(brazoIzqReposo);
  servoBrazoDer.write(brazoDerReposo);
  servoCabeza.write(cabezaCenGrados);

  Serial.begin(115200);
  SerialBT.begin(bluetoothName);

  // Semilla para secuencias aleatorias de Explorar
  randomSeed(analogRead(0) ^ (millis() << 8));

  setupButtons();
  lcdReady = initLcd();
  setMode(DisplayMode::Standby);

  Serial.println("Bluetooth listo. Comandos: F B L R S G H X D V P | STANDBY DRIVING MODE1 MODE2 | ON OFF | TEXT: SET_HORA: SET_TIEMPO: SET_NOMBRE: | BRAZOS CABEZA_IZQ CABEZA_CEN CABEZA_DER");
}

void loop() {
  readBluetoothCommands();
  processTelemetryButtons();
  updateMode1Scroll();
  updateSpecialMove();
}
