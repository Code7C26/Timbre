}// Sistema de Timbre Automatizado - Arquitectura Asincrona

#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>

// =========================================================================
// 1. INICIALIZACION DE HARDWARE Y PINES
// =========================================================================

LiquidCrystal_I2C lcd(0x27, 16, 2); 
RTC_DS3231 RTC; 

const int btnOk     = 6;  
const int timbre    = 7;  
const int btnMenu   = 8;  
const int btnUp     = 9;  
const int btnDown   = 10; 
const int btnPause  = 11; 
const int btnSalir  = 12; // Nuevo boton para escape rapido

// =========================================================================
// 2. ESTRUCTURAS DE DATOS (ALARMAS)
// =========================================================================

struct AlarmaFija {
  int hora;
  int minuto;
  int tipo; 
};

const AlarmaFija horarios[] = {
  {7, 40, 1}, {7, 45, 1}, {8, 25, 3}, {9, 5, 1},
  {9, 20, 2}, {9, 55, 3}, {10, 35, 1}, {10, 45, 2},
  {11, 25, 3}, {12, 5, 1}, {12, 10, 2}, {12, 50, 1},
  {13, 30, 4}, {13, 40, 1}, {13, 45, 1}, {14, 25, 3},
  {15, 5, 1}, {15, 15, 2}, {15, 55, 3}, {16, 35, 1},
  {16, 45, 2}, {17, 25, 3}, {18, 0, 1}
};
const int numAlarmasFijas = sizeof(horarios) / sizeof(horarios[0]);

struct AlarmaPuntual {
  int hora;
  int minuto;
  int tipo;
  bool activa; 
};
AlarmaPuntual timbreEspecial = {0, 0, 1, false}; 
const char* nombresTipos[] = {"Largo", "Muy Largo", "Pulsos", "Corto"};

// =========================================================================
// 3. VARIABLES DE ESTADO Y CONTROL ASINCRONO
// =========================================================================

enum AppState { NORMAL, MAIN_MENU, EDIT_CLOCK, EDIT_SPECIAL };
AppState currentState = NORMAL;

int menuSelection = 0; 
int editStep = 0;      
int tYear, tMonth, tDay, tHour, tMin;
int tsHour, tsMin, tsTypeIndex;

bool isGlobalPaused = false; 
int lastRungMinute = -1;     
int lastDisplaySecond = -1;  
bool lcdNeedsUpdate = true;  

bool isRinging = false;
int currentRingType = 0;
unsigned long ringStartMillis = 0;  
unsigned long pulseStartMillis = 0; 
int pulseCount = 0;
bool pulseState = false;

// Arrays actualizados para soportar 6 botones
const int numButtons = 6; 
int btnPins[numButtons] = {btnOk, btnMenu, btnUp, btnDown, btnPause, btnSalir};
bool lastBtnReadings[numButtons] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
bool validatedBtnStates[numButtons] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[numButtons] = {0, 0, 0, 0, 0, 0};
const unsigned long debounceDelay = 50; 

// =========================================================================
// CONFIGURACION INICIAL 
// =========================================================================
void setup() {
  pinMode(timbre, OUTPUT);
  digitalWrite(timbre, LOW); 

  for (int i = 0; i < numButtons; i++) { pinMode(btnPins[i], INPUT_PULLUP); }

  Wire.begin();
  RTC.begin();
  
  lcd.init();
  lcd.backlight();
}

// =========================================================================
// BUCLE PRINCIPAL 
// =========================================================================
void loop() {
  DateTime now = RTC.now();              
  unsigned long currentMillis = millis();

  manejarBotones(now);
  evaluarTimbres(now);
  actualizarRele(currentMillis);
  actualizarPantalla(now);
}

// =========================================================================
// LECTURA DE BOTONES
// =========================================================================
bool isButtonPressed(int btnIndex) {
  bool reading = digitalRead(btnPins[btnIndex]);
  bool isPressed = false;

  if (reading != lastBtnReadings[btnIndex]) lastDebounceTime[btnIndex] = millis();

  if ((millis() - lastDebounceTime[btnIndex]) > debounceDelay) {
    if (reading != validatedBtnStates[btnIndex]) {
      validatedBtnStates[btnIndex] = reading;
      if (validatedBtnStates[btnIndex] == LOW) isPressed = true;
    }
  }
  lastBtnReadings[btnIndex] = reading;
  return isPressed;
}

void manejarBotones(DateTime now) {
  bool btnOkPressed    = isButtonPressed(0);
  bool btnMenuPressed  = isButtonPressed(1);
  bool btnUpPressed    = isButtonPressed(2);
  bool btnDownPressed  = isButtonPressed(3);
  bool btnPausePressed = isButtonPressed(4); 
  bool btnSalirPressed = isButtonPressed(5); // Lectura del nuevo boton

  // Accion de escape rapido
  if (btnSalirPressed && currentState != NORMAL) {
    currentState = NORMAL;
    lcdNeedsUpdate = true;
    return; // Detiene la evaluacion de los demas botones en este ciclo
  }

  if (btnPausePressed) {
    if (currentState == NORMAL) {
      isGlobalPaused = !isGlobalPaused; 
      if (isGlobalPaused && isRinging) detenerTimbre(); 
    }
    lcdNeedsUpdate = true;
  }

  switch (currentState) {
    case NORMAL: 
      if (btnMenuPressed) {
        currentState = MAIN_MENU;
        menuSelection = 0;
        lcdNeedsUpdate = true;
      }
      if (btnOkPressed && timbreEspecial.activa) {
        timbreEspecial.activa = false;
        lcdNeedsUpdate = true;
      }
      break;

    case MAIN_MENU: 
      if (btnMenuPressed || btnUpPressed || btnDownPressed) {
        menuSelection = !menuSelection; 
        lcdNeedsUpdate = true;
      }
      if (btnOkPressed) {
        if (menuSelection == 0) {
          currentState = EDIT_CLOCK;
          editStep = 0;
          tYear = now.year(); tMonth = now.month(); tDay = now.day();
          tHour = now.hour(); tMin = now.minute(); 
        } else {
          currentState = EDIT_SPECIAL;
          editStep = 0;
          tsHour = now.hour(); tsMin = now.minute(); tsTypeIndex = 0;
        }
        lcdNeedsUpdate = true;
      }
      break;

    case EDIT_CLOCK: 
      if (btnMenuPressed) {
        editStep = (editStep + 1) % 5; 
        lcdNeedsUpdate = true;
      }
      if (btnUpPressed)  ajustarReloj(1);
      if (btnDownPressed) ajustarReloj(-1);
      
      if (btnOkPressed) { 
        RTC.adjust(DateTime(tYear, tMonth, tDay, tHour, tMin, 0)); 
        currentState = NORMAL; 
        lcdNeedsUpdate = true;
      }
      break;

    case EDIT_SPECIAL: 
      if (btnMenuPressed) {
        editStep = (editStep + 1) % 3; 
        lcdNeedsUpdate = true;
      }
      if (btnUpPressed)  ajustarPuntual(1);
      if (btnDownPressed) ajustarPuntual(-1);

      if (btnOkPressed) { 
        timbreEspecial.hora = tsHour;
        timbreEspecial.minuto = tsMin;
        timbreEspecial.tipo = tsTypeIndex + 1; 
        timbreEspecial.activa = true; 
        currentState = NORMAL; 
        lcdNeedsUpdate = true;
      }
      break;
  }
}

// =========================================================================
// LOGICA DE DISPARO DE TIMBRES
// =========================================================================
void evaluarTimbres(DateTime now) {
  if (isGlobalPaused) return; 

  if (now.second() == 0 && lastRungMinute != now.minute()) {
    bool disparo = false;

    if (timbreEspecial.activa && timbreEspecial.hora == now.hour() && timbreEspecial.minuto == now.minute()) {
      iniciarTimbre(timbreEspecial.tipo);
      timbreEspecial.activa = false; 
      disparo = true;
    }

    if (!disparo) {
      for (int i = 0; i < numAlarmasFijas; i++) {
        if (horarios[i].hora == now.hour() && horarios[i].minuto == now.minute()) {
          iniciarTimbre(horarios[i].tipo);
          disparo = true;
          break; 
        }
      }
    }
    
    if (disparo) lastRungMinute = now.minute(); 
  }
}

void iniciarTimbre(int tipo) {
  if (isRinging) return; 
  isRinging = true;
  currentRingType = tipo;
  ringStartMillis = millis(); 
  pulseStartMillis = millis();
  pulseCount = 0;
  pulseState = true;
  digitalWrite(timbre, HIGH); 
  lcdNeedsUpdate = true;
}

void detenerTimbre() {
  isRinging = false;
  digitalWrite(timbre, LOW); 
  lcdNeedsUpdate = true;
}

void actualizarRele(unsigned long currentMillis) {
  if (!isRinging) return;

  switch (currentRingType) {
    case 1: if (currentMillis - ringStartMillis >= 8000) detenerTimbre(); break;  
    case 2: if (currentMillis - ringStartMillis >= 10000) detenerTimbre(); break; 
    case 3: 
      if (currentMillis - pulseStartMillis >= 1000) { 
        pulseStartMillis = currentMillis;
        pulseState = !pulseState;
        digitalWrite(timbre, pulseState ? HIGH : LOW);
        if (!pulseState) pulseCount++;
        if (pulseCount >= 3) detenerTimbre();
      }
      break;
    case 4: if (currentMillis - ringStartMillis >= 2000) detenerTimbre(); break;  
  }
}

// =========================================================================
// FUNCIONES DE PANTALLA LCD
// =========================================================================
void print2(int number) {
  if (number < 10) lcd.print("0");
  lcd.print(number);
}

void drawIndicator(int currentStep, int targetStep) {
  if (currentStep == targetStep) lcd.print(">");
  else lcd.print(" ");
}

void actualizarPantalla(DateTime now) {
  if (currentState == NORMAL && now.second() != lastDisplaySecond) {
    lcdNeedsUpdate = true;
    lastDisplaySecond = now.second();
  }

  if (!lcdNeedsUpdate) return;
  lcdNeedsUpdate = false;
  lcd.clear(); 

  if (isRinging) {
    lcd.setCursor(0, 0);
    lcd.print("   Sonando...   ");
    return; 
  }

  switch (currentState) {
    case NORMAL:
      lcd.setCursor(0, 0);
      lcd.print(now.year()); lcd.print("/"); print2(now.month()); lcd.print("/"); print2(now.day());
      
      lcd.setCursor(11, 0);
      if (isGlobalPaused) lcd.print("PAUSA");
      
      lcd.setCursor(0, 1);
      lcd.print("Hora: "); print2(now.hour()); lcd.print(":"); print2(now.minute()); lcd.print(":"); print2(now.second());
      
      if(timbreEspecial.activa) {
        lcd.setCursor(15, 1); lcd.print("*");
      }
      break;

    case MAIN_MENU:
      lcd.setCursor(0, 0); lcd.print("Menu Principal:");
      lcd.setCursor(0, 1);
      if (menuSelection == 0) lcd.print("[Ajustar Reloj] ");
      else                    lcd.print("[Timbre Unico]  ");
      break;

    case EDIT_CLOCK:
      lcd.setCursor(0, 0); 
      drawIndicator(editStep, 0); lcd.print(tYear); lcd.print("/"); 
      drawIndicator(editStep, 1); print2(tMonth); lcd.print("/"); 
      drawIndicator(editStep, 2); print2(tDay);
      
      lcd.setCursor(0, 1);
      drawIndicator(editStep, 3); print2(tHour); lcd.print(":"); 
      drawIndicator(editStep, 4); print2(tMin);
      break;

    case EDIT_SPECIAL:
      lcd.setCursor(0, 0); lcd.print("Timbre Puntual:");
      lcd.setCursor(0, 1);
      if (editStep == 0) {
        lcd.print(">Hora: "); print2(tsHour);
      } else if (editStep == 1) {
        lcd.print(">Minuto: "); print2(tsMin);
      } else if (editStep == 2) {
        lcd.print(">Tipo: "); lcd.print(nombresTipos[tsTypeIndex]);
      }
      break;
  }
}

// =========================================================================
// MATEMATICAS DE NAVEGACION DE MENU
// =========================================================================
void ajustarReloj(int delta) {
  if (editStep == 0) tYear += delta;
  if (editStep == 1) { tMonth += delta; if(tMonth < 1) tMonth = 12; else if(tMonth > 12) tMonth = 1; }
  if (editStep == 2) { tDay += delta; if(tDay < 1) tDay = 31; else if(tDay > 31) tDay = 1; }
  if (editStep == 3) { tHour += delta; if(tHour < 0) tHour = 23; else if(tHour > 23) tHour = 0; }
  if (editStep == 4) { tMin += delta; if(tMin < 0) tMin = 59; else if(tMin > 59) tMin = 0; }
  lcdNeedsUpdate = true;
}

void ajustarPuntual(int delta) {
  if (editStep == 0) { tsHour += delta; if(tsHour < 0) tsHour = 23; else if(tsHour > 23) tsHour = 0; }
  if (editStep == 1) { tsMin += delta; if(tsMin < 0) tsMin = 59; else if(tsMin > 59) tsMin = 0; }
  if (editStep == 2) { 
    tsTypeIndex += delta; 
    if(tsTypeIndex < 0) tsTypeIndex = 3; 
    else if(tsTypeIndex > 3) tsTypeIndex = 0; 
  }
  lcdNeedsUpdate = true;
}