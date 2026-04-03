/*
  FM Transmitter — SI4713 Clone (I2C address 0x11)
  -------------------------------------------------------
  This system scans the FM band for the three quietest
  frequencies, displays the top two on an LCD, then allows
  the user to tune a transmission frequency via two
  potentiometers. A button interrupt confirms and sends the
  selected frequency to the SI4713 FM transmitter chip.

  Hardware:
  - Arduino Uno
  - SI4713 FM transmitter (clone, address 0x11)
  - 16x2 LCD with I2C backpack
  - Two potentiometers (tens digit A3, decimal digit A2)
  - LM311 comparator + button on interrupt pin 2 (INT0)
  - MAX9814 electret microphone → SI4713 Lin pin
  - ~70cm antenna wire on SI4713 ANT pin

  Key design decisions:
  - SI4713 clone requires raw I2C POWER_UP command (0x01)
    before any library calls — standard Adafruit begin()
    does not correctly initialize this clone
  - All SI4713 commands sent raw over I2C, no library used
  - Button debounced in software using timestamp comparison
*/
#include <Wire.h>
#include <LiquidCrystal.h>
#define DEBOUNCE_MS 100 // minimum ms between valid button presses
#define RESETPIN 12 // SI4713 RST pin — held LOW then HIGH to reset chip
#define SI4713_ADDR 0x11 // I2C address of clone SI4713 (genuine Adafruit = 0x63)

// timing variable for non-blocking loop updates
long lmillis = 0;
// current FM station in units of 10 kHz (e.g. 10230 = 102.3 MHz)
int FMSTATION = 10230;

// LCD pin assignments (using direct 4-bit mode, not I2C backpack)
const int rs = 8, en = 9, d4 = 4, d5 = 5, d6 = 6, d7 = 7;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// potentiometer pin assignments
// inPotTens controls the integer MHz range (88-114 MHz)
// inPotDec controls the decimal portion (0.00-1.27 MHz)
const int inPotTens = A3;
const int inPotDec = A2;
int vPotTens = 0;
int vPotDec = 0;
float fullPot = 0; // combined frequency reading in MHz (e.g. 102.30)

// interrupt and debounce variables
// volatile because modified inside ISR and read in main loop
volatile int sendFreq = 0; // flag set by ISR to trigger frequency update
volatile int count = 0; // total confirmed button press count
volatile unsigned long lastInterruptTime = 0; // timestamp of last valid press
int lastCount = -1; // used to detect count changes for Serial printing

// -------------------------------------------------------
// sendCommand — sends a raw I2C command to the SI4713
// The SI4713 datasheet specifies command byte followed by
// up to 6 argument bytes. Unused args default to 0x00.
// -------------------------------------------------------
void sendCommand(uint8_t cmd, uint8_t a=0, uint8_t b=0, uint8_t c=0, uint8_t d=0, uint8_t e=0) {
  Wire.beginTransmission(SI4713_ADDR);
  Wire.write(cmd);
  Wire.write(a); Wire.write(b); Wire.write(c);
  Wire.write(d); Wire.write(e);
  Wire.endTransmission();
  delay(100); // allow chip time to process command
}

// -------------------------------------------------------
// getStatus — reads the 1-byte status register from SI4713
// 0x80 = command completed successfully (CTS bit set)
// Any other value indicates the chip is busy or errored
// -------------------------------------------------------
uint8_t getStatus() {
  Wire.requestFrom(SI4713_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0;
}

// -------------------------------------------------------
// tuneTo — tunes the SI4713 to a given frequency
// Uses TX_TUNE_FREQ command (0x30)
// freq is in units of 10 kHz (e.g. 10230 = 102.3 MHz)
// The frequency is split into high and low bytes for I2C
// -------------------------------------------------------
void tuneTo(uint16_t freq) {
  uint8_t high = freq >> 8; // upper byte of frequency
  uint8_t low  = freq & 0xFF; // lower byte of frequency
  sendCommand(0x30, 0x00, high, low);
}

// -------------------------------------------------------
// readASQ — reads Audio Signal Quality status from SI4713
// Uses TX_ASQ_STATUS command (0x34)
// ASQ byte flags: 0x01 = audio overmodulating (too loud)
//                 0x02 = audio below threshold (too quiet)
// inLevel is signed dBFS — 0 = full scale, -60 = silence
// -------------------------------------------------------
void readASQ() {
  sendCommand(0x34, 0x01);
  Wire.requestFrom(SI4713_ADDR, 5);
  uint8_t status = 0, asq = 0, inLevel = 0;
  if (Wire.available()) status  = Wire.read();
  if (Wire.available()) asq     = Wire.read();
  Wire.read(); Wire.read(); //skip two unused bytes
  if (Wire.available()) inLevel = Wire.read();
  Serial.print("ASQ: 0x");       Serial.println(asq, HEX);
  Serial.print("Input level: "); Serial.println((int8_t)inLevel);
}

// -------------------------------------------------------
// readTuneStatus — reads current transmit status from SI4713
// Uses TX_TUNE_STATUS command (0x33)
// Returns: current frequency, transmit power in dBuV,
// and antenna capacitor tuning value
// Power = 115 dBuV confirms chip is transmitting at max
// -------------------------------------------------------
void readTuneStatus() {
  sendCommand(0x33, 0x01);
  Wire.requestFrom(SI4713_ADDR, 8);
  uint8_t bytes[8] = {0};
  for (int i = 0; i < 8 && Wire.available(); i++) bytes[i] = Wire.read();
  uint16_t freq  = ((uint16_t)bytes[2] << 8) | bytes[3]; // reconstruct 16 bit freq
  uint8_t  power = bytes[5]; // transmit power in dBuV (88-115)
  uint8_t  antcap = bytes[6]; // antenna capacitor value (0 = auto)
  Serial.print("Curr freq: ");    Serial.println(freq);
  Serial.print("Power (dBuV): "); Serial.println(power);
  Serial.print("ANTcap: ");       Serial.println(antcap);
}

void setup() {
  Serial.begin(9600);
  // attach button interrupt on pin 2 (INT0), triggers on FALLING edge
  // LM311 comparator outputs a clean digital FALLING transition on button press
  attachInterrupt(0, tune, FALLING);
  lcd.begin(16, 2);
  pinMode(inPotTens, INPUT);
  //startup message while chip initializes
  lcd.setCursor(0, 0);
  lcd.print("SCANNING FOR ");
  lcd.setCursor(0, 1);
  lcd.print("BEST STATIONS...");

  // hardware reset sequence for SI4713
  // RST must be held LOW briefly then released HIGH to guarantee
  // the chip initializes from a known state on every power cycle
  pinMode(RESETPIN, OUTPUT);
  digitalWrite(RESETPIN, LOW);
  delay(100);
  digitalWrite(RESETPIN, HIGH);
  delay(100);

  Wire.begin();
  delay(100);

  // raw POWER_UP command required for clone SI4713 chips
  // 0x01 = POWER_UP, 0x12 = TX mode + crystal oscillator,
  // 0x50 = analog audio input mode
  // The Adafruit library's begin() does not send this correctly
  // for clone chips — sending it raw first guarantees the chip
  // wakes up and accepts subsequent commands
  sendCommand(0x01, 0x12, 0x50);
  Serial.print("Power up status: 0x"); Serial.println(getStatus(), HEX);

  // set transmit power to maximum (115 dBuV)
  // TX_TUNE_POWER command (0x31): arg3 = power level 88-115
  sendCommand(0x31, 0x00, 0x00, 115, 0x00);
  Serial.print("Set power status: 0x"); Serial.println(getStatus(), HEX);

  // -------------------------------------------------------
  // FM band scan — find 3 quietest frequencies
  // Scans 88.0 to 108.0 MHz in 100 kHz steps
  // For each frequency, reads noise level from tune status
  // Lower noise = cleaner channel to transmit on
  // Stores top 3 results in bestFreq/bestNoise arrays
  // -------------------------------------------------------
  uint16_t bestFreq[3]  = {0, 0, 0};
  uint8_t  bestNoise[3] = {255, 255, 255}; // 255 = worst, gets replaced instantly

  for (uint16_t f = 8800; f <= 10800; f += 10) {
    // TX_TUNE_MEASURE
    uint8_t high = f >> 8;
    uint8_t low  = f & 0xFF;
    sendCommand(0x30, 0x00, high, low); // tune to this frequency to measure it
    delay(5);

    // read back tune status to get noise level at this frequency
    sendCommand(0x33, 0x01);
    Wire.requestFrom(SI4713_ADDR, 8);
    uint8_t bytes[8] = {0};
    for (int i = 0; i < 8 && Wire.available(); i++) bytes[i] = Wire.read();
    uint8_t noise = bytes[7]; // noise floor at this frequency

    // insertion sort into top 3 — keeps best (lowest noise) candidates
    if (noise < bestNoise[0]) {
      bestNoise[2] = bestNoise[1]; bestFreq[2] = bestFreq[1];
      bestNoise[1] = bestNoise[0]; bestFreq[1] = bestFreq[0];
      bestNoise[0] = noise;        bestFreq[0] = f;
    } else if (noise < bestNoise[1]) {
      bestNoise[2] = bestNoise[1]; bestFreq[2] = bestFreq[1];
      bestNoise[1] = noise;        bestFreq[1] = f;
    } else if (noise < bestNoise[2]) {
      bestNoise[2] = noise;        bestFreq[2] = f;
    }
  }

  // display top 2 quietest frequencies and their noise levels
  // format: "XX.XX N:YY" where XX.XX = MHz, YY = noise value
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(bestFreq[0] / 100.0, 2);
  lcd.print(" N:"); lcd.print(bestNoise[0]);
  lcd.setCursor(0, 1);
  lcd.print(bestFreq[1] / 100.0, 2);
  lcd.print(" N:"); lcd.print(bestNoise[1]);
  delay(2000); //show results for 2 secs

  // set up main LCD display layout
  // row 0: TUNE shows currently selected pot frequency (not yet sent)
  // row 1: SEND shows the frequency being actively transmitted
  lcd.setCursor(0, 0);
  lcd.print("TUNE: x.xx   MHz");
  lcd.setCursor(0, 1);
  lcd.print("SEND: x.xx   MHz");

  // read potentiometers and compute initial frequency
  // inPotTens maps 0-1023 → 88-114 (integer MHz range)
  // inPotDec maps 0-1023 → 0-127 (decimal portion in 0.01 MHz steps)
  vPotTens = (26.0 * analogRead(inPotTens) / 1024) + 88;
  vPotDec  = (128.0 * analogRead(inPotDec) / 1024);
  fullPot  = (float)vPotTens + (vPotDec / 100.0);

  lcd.setCursor(6, 0);
  lcd.print(fullPot); lcd.print(" ");
  lcd.setCursor(6, 1);
  lcd.print(fullPot); lcd.print(" ");

  // convert float MHz to integer 10 kHz units for SI4713
  FMSTATION = (int)(fullPot * 100.0);
  tuneTo(FMSTATION);
  Serial.print("Tuning into "); Serial.println(FMSTATION);

  readTuneStatus();
  Serial.println("Transmitting! Speak into the mic.");
}

void loop() {
  // print count to Serial only when it changes
  // confirms each button press registered exactly once (debounce check)
  if (count != lastCount) {
    Serial.print("count: "); Serial.println(count);
    lastCount = count;
  }

  // continuously read potentiometers and update TUNE row on LCD
  // this gives real time feedback as the user adjusts the frequency
  // before committing it with the button
  vPotTens = (26.0 * analogRead(inPotTens) / 1024) + 88;
  vPotDec  = (128.0 * analogRead(inPotDec) / 1024);
  fullPot  = (float)vPotTens + (vPotDec / 100.0);
  lcd.setCursor(6, 1);
  lcd.print(fullPot); lcd.print(" ");

  // sendFreq flag set by ISR when button is pressed
  // when set, commit the current pot frequency to the transmitter
  if (sendFreq == 1) {
    lcd.clear();
    lcd.setCursor(0, 0);

    // convert and send new frequency to SI4713
    FMSTATION = (int)(fullPot * 100.0);
    tuneTo(FMSTATION);
    delay(100);

    // confirm new frequency via Serial and LCD
    readTuneStatus();
    lcd.print("NEW FREQ SET");
    Serial.print("New freq: "); Serial.println(FMSTATION);
    delay(1000);

    // restore LCD display layout after confirmation message
    lcd.setCursor(0, 0);
    lcd.print("TUNE: x.xx   MHz");
    lcd.setCursor(0, 1);
    lcd.print("SEND: x.xx   MHz");
    lcd.setCursor(6, 0);
    lcd.print(fullPot); lcd.print(" ");
    lcd.setCursor(6, 1);
    lcd.print(fullPot); lcd.print(" ");

    sendFreq = 0; // reset flag - ready for next button press
  }

  // print transmitter diagnostics every 2 seconds
  // ASQ confirms mic audio is present and within acceptable range
  // power should read 115 dBuV confirming chip is transmitting
  if (lmillis <= millis()) {
    lmillis = millis() + 2000;
    readASQ();
    readTuneStatus();
    Serial.println("----------");
  }

  delay(100); // short loop delay for visibility on LCD
}

// -------------------------------------------------------
// tune — ISR triggered on FALLING edge of interrupt pin 2
// LM311 comparator produces clean FALLING edge on button press
// Software debounce ignores any trigger within DEBOUNCE_MS
// of the last valid press to eliminate mechanical bounce
// Sets sendFreq flag for main loop to handle frequency update
// ISR kept minimal — no delays, no Serial, no LCD calls
// -------------------------------------------------------
void tune() {
  unsigned long now = millis();
  if (now - lastInterruptTime > DEBOUNCE_MS) {
    sendFreq = 1; // signal main loop to update frequency
    count = count + 1; // increment press counter for debugging
    lastInterruptTime = now; // record time of this valid press
  }
}