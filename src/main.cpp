// Shiny AVR Programmer
//
// Based on ArduinoISP, original copyright (c) 2008-2011 Randall Bohn
// https://github.com/arduino/arduino-examples/blob/main/examples/11.ArduinoISP/ArduinoISP/ArduinoISP.ino

#include "Arduino.h"

// STM32 has hardware ISP which is way too fast for most AVR chips
// that's why a bitbanged implementation is used instead.
#define SPI_CLOCK (1000000 / 6)

// Configure which pins to use:
#define RESET PA10
#define LED_STATUS PA1

#define PIN_MOSI PA7
#define PIN_MISO PA6
#define PIN_SCK PA5
#define PIN_EXT_CLOCK PA8

#define BAUDRATE 19200

#define HWVER 2
#define SWMAJ 1
#define SWMIN 18

// STK Definitions
#define STK_OK 0x10
#define STK_FAILED 0x11
#define STK_UNKNOWN 0x12
#define STK_INSYNC 0x14
#define STK_NOSYNC 0x15
#define CRC_EOP 0x20 // ok it is a space...

void pulse(int pin, int times);
void avrisp();

#define SPI_MODE0 0x00

class SPISettings {
public:
  // clock is in Hz
  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
      : clock(clock) {
    (void)bitOrder;
    (void)dataMode;
  };

private:
  uint32_t clock;

  friend class BitBangedSPI;
};

class BitBangedSPI {
public:
  void begin() {
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, LOW);
    pinMode(PIN_SCK, OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_MISO, INPUT);
  }

  void beginTransaction(SPISettings settings) {
    pulseWidth = (500000 + settings.clock - 1) / settings.clock;
    if (pulseWidth == 0)
      pulseWidth = 1;
  }

  void end() {}

  uint8_t transfer(uint8_t b) {
    for (unsigned int i = 0; i < 8; ++i) {
      digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
      digitalWrite(PIN_SCK, HIGH);
      delayMicroseconds(pulseWidth);
      b = (b << 1) | digitalRead(PIN_MISO);
      digitalWrite(PIN_SCK, LOW); // slow pulse
      delayMicroseconds(pulseWidth);
    }
    return b;
  }

private:
  unsigned long pulseWidth; // in microseconds
};

static BitBangedSPI SPI;

void setup() {
  Serial.begin(BAUDRATE);

  pinMode(LED_STATUS, OUTPUT);
  pulse(LED_STATUS, 2);
}

int error = 0;
int pmode = 0;
// address for reading and writing, set by 'U' command
unsigned int here;
uint8_t buff[256]; // global block storage

#define beget16(addr) (*addr * 256 + *(addr + 1))
typedef struct param {
  uint8_t devicecode;
  uint8_t revision;
  uint8_t progtype;
  uint8_t parmode;
  uint8_t polling;
  uint8_t selftimed;
  uint8_t lockbytes;
  uint8_t fusebytes;
  uint8_t flashpoll;
  uint16_t eeprompoll;
  uint16_t pagesize;
  uint16_t eepromsize;
  uint32_t flashsize;
} parameter;

parameter param;

static bool rst_active_high;

void reset_target(bool reset) {
  digitalWrite(RESET,
               ((reset && rst_active_high) || (!reset && !rst_active_high))
                   ? HIGH
                   : LOW);
}

void loop(void) {
  if (error) {
    digitalWrite(LED_STATUS, (millis() % 500) < 250);
  } else if (pmode) {
    digitalWrite(LED_STATUS, HIGH);
  } else {
    digitalWrite(LED_STATUS, LOW);
  }

  if (Serial.available()) {
    avrisp();
  }
}

uint8_t getch() {
  while (!Serial.available())
    ;
  return Serial.read();
}
void fill(int n) {
  for (int x = 0; x < n; x++) {
    buff[x] = getch();
  }
}

#define PTIME 30
void pulse(int pin, int times) {
  do {
    digitalWrite(pin, HIGH);
    delay(PTIME);
    digitalWrite(pin, LOW);
    delay(PTIME);
  } while (times--);
}

uint8_t spi_transaction(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  SPI.transfer(a);
  SPI.transfer(b);
  SPI.transfer(c);
  return SPI.transfer(d);
}

void empty_reply() {
  if (CRC_EOP == getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)STK_OK);
  } else {
    error++;
    Serial.print((char)STK_NOSYNC);
  }
}

void breply(uint8_t b) {
  if (CRC_EOP == getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)b);
    Serial.print((char)STK_OK);
  } else {
    error++;
    Serial.print((char)STK_NOSYNC);
  }
}

void get_version(uint8_t c) {
  switch (c) {
  case 0x80:
    breply(HWVER);
    break;
  case 0x81:
    breply(SWMAJ);
    break;
  case 0x82:
    breply(SWMIN);
    break;
  case 0x93:
    breply('S'); // serial programmer
    break;
  default:
    breply(0);
  }
}

void set_parameters() {
  // call this after reading parameter packet into buff[]
  param.devicecode = buff[0];
  param.revision = buff[1];
  param.progtype = buff[2];
  param.parmode = buff[3];
  param.polling = buff[4];
  param.selftimed = buff[5];
  param.lockbytes = buff[6];
  param.fusebytes = buff[7];
  param.flashpoll = buff[8];
  // ignore buff[9] (= buff[8])
  // following are 16 bits (big endian)
  param.eeprompoll = beget16(&buff[10]);
  param.pagesize = beget16(&buff[12]);
  param.eepromsize = beget16(&buff[14]);

  // 32 bits flashsize (big endian)
  param.flashsize = buff[16] * 0x01000000 + buff[17] * 0x00010000 +
                    buff[18] * 0x00000100 + buff[19];

  // AVR devices have active low reset, AT89Sx are active high
  rst_active_high = (param.devicecode >= 0xe0);
}

void start_pmode() {

  // Reset target before driving PIN_SCK or PIN_MOSI

  // SPI.begin() will configure SS as output, so SPI master mode is selected.
  // We have defined RESET as pin 10, which for many Arduinos is not the SS pin.
  // So we have to configure RESET as output here,
  // (reset_target() first sets the correct level)
  reset_target(true);

  // Start the external clock at 16 MHz for ATtinys that require external clock or crystal
  pinMode(PIN_EXT_CLOCK, OUTPUT);
  analogWriteResolution(2);
  analogWriteFrequency(16000000);
  analogWrite(PIN_EXT_CLOCK, 1);
  pinMode(RESET, OUTPUT);
  
  SPI.begin();
  SPI.beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));

  // See AVR datasheets, chapter "Serial_PRG Programming Algorithm":

  // Pulse RESET after PIN_SCK is low:
  digitalWrite(PIN_SCK, LOW);
  delay(20); // discharge PIN_SCK, value arbitrarily chosen
  reset_target(false);
  // Pulse must be minimum 2 target CPU clock cycles so 100 usec is ok for CPU
  // speeds above 20 KHz
  delayMicroseconds(100);
  reset_target(true);

  // Send the enable programming command:
  delay(50); // datasheet: must be > 20 msec
  spi_transaction(0xAC, 0x53, 0x00, 0x00);
  pmode = 1;
}

void end_pmode() {
  SPI.end();
  // We're about to take the target out of reset so configure SPI pins as input
  pinMode(PIN_MOSI, INPUT);
  pinMode(PIN_SCK, INPUT);
  pinMode(PIN_EXT_CLOCK, INPUT);

  reset_target(false);
  pinMode(RESET, INPUT);
  pmode = 0;
}

void universal() {
  uint8_t ch;

  fill(4);
  ch = spi_transaction(buff[0], buff[1], buff[2], buff[3]);
  breply(ch);
}

void flash(uint8_t hilo, unsigned int addr, uint8_t data) {
  spi_transaction(0x40 + 8 * hilo, addr >> 8 & 0xFF, addr & 0xFF, data);
}
void commit(unsigned int addr) {
  spi_transaction(0x4C, (addr >> 8) & 0xFF, addr & 0xFF, 0);

  // This delay is important, otherwise we get verification errors with avrdude
  delay(PTIME);
}

unsigned int current_page() {
  if (param.pagesize == 32) {
    return here & 0xFFFFFFF0;
  }
  if (param.pagesize == 64) {
    return here & 0xFFFFFFE0;
  }
  if (param.pagesize == 128) {
    return here & 0xFFFFFFC0;
  }
  if (param.pagesize == 256) {
    return here & 0xFFFFFF80;
  }
  return here;
}

uint8_t write_flash_pages(int length);

void write_flash(int length) {
  fill(length);
  if (CRC_EOP == getch()) {
    Serial.print((char)STK_INSYNC);
    Serial.print((char)write_flash_pages(length));
  } else {
    error++;
    Serial.print((char)STK_NOSYNC);
  }
}

uint8_t write_flash_pages(int length) {
  int x = 0;
  unsigned int page = current_page();
  while (x < length) {
    if (page != current_page()) {
      commit(page);
      page = current_page();
    }
    flash(LOW, here, buff[x++]);
    flash(HIGH, here, buff[x++]);
    here++;
  }

  commit(page);

  return STK_OK;
}

uint8_t write_eeprom_chunk(unsigned int start, unsigned int length);

#define EECHUNK (32)

uint8_t write_eeprom(unsigned int length) {
  // here is a word address, get the byte address
  unsigned int start = here * 2;
  unsigned int remaining = length;
  if (length > param.eepromsize) {
    error++;
    return STK_FAILED;
  }
  while (remaining > EECHUNK) {
    write_eeprom_chunk(start, EECHUNK);
    start += EECHUNK;
    remaining -= EECHUNK;
  }
  write_eeprom_chunk(start, remaining);
  return STK_OK;
}
// write (length) bytes, (start) is a byte address
uint8_t write_eeprom_chunk(unsigned int start, unsigned int length) {
  // this writes byte-by-byte, page writing may be faster (4 bytes at a time)
  fill(length);
  for (unsigned int x = 0; x < length; x++) {
    unsigned int addr = start + x;
    spi_transaction(0xC0, (addr >> 8) & 0xFF, addr & 0xFF, buff[x]);
    delay(45);
  }
  return STK_OK;
}

void program_page() {
  char result = (char)STK_FAILED;
  unsigned int length = 256 * getch();
  length += getch();
  char memtype = getch();
  // flash memory @here, (length) bytes
  if (memtype == 'F') {
    write_flash(length);
    return;
  }
  if (memtype == 'E') {
    result = (char)write_eeprom(length);
    if (CRC_EOP == getch()) {
      Serial.print((char)STK_INSYNC);
      Serial.print(result);
    } else {
      error++;
      Serial.print((char)STK_NOSYNC);
    }
    return;
  }
  Serial.print((char)STK_FAILED);
  return;
}

uint8_t flash_read(uint8_t hilo, unsigned int addr) {
  return spi_transaction(0x20 + hilo * 8, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

char flash_read_page(int length) {
  for (int x = 0; x < length; x += 2) {
    uint8_t low = flash_read(LOW, here);
    Serial.print((char)low);
    uint8_t high = flash_read(HIGH, here);
    Serial.print((char)high);
    here++;
  }
  return STK_OK;
}

char eeprom_read_page(int length) {
  // here again we have a word address
  int start = here * 2;
  for (int x = 0; x < length; x++) {
    int addr = start + x;
    uint8_t ee = spi_transaction(0xA0, (addr >> 8) & 0xFF, addr & 0xFF, 0xFF);
    Serial.print((char)ee);
  }
  return STK_OK;
}

void read_page() {
  char result = (char)STK_FAILED;
  int length = 256 * getch();
  length += getch();
  char memtype = getch();
  if (CRC_EOP != getch()) {
    error++;
    Serial.print((char)STK_NOSYNC);
    return;
  }
  Serial.print((char)STK_INSYNC);
  if (memtype == 'F')
    result = flash_read_page(length);
  if (memtype == 'E')
    result = eeprom_read_page(length);
  Serial.print(result);
}

void read_signature() {
  if (CRC_EOP != getch()) {
    error++;
    Serial.print((char)STK_NOSYNC);
    return;
  }
  Serial.print((char)STK_INSYNC);
  uint8_t high = spi_transaction(0x30, 0x00, 0x00, 0x00);
  Serial.print((char)high);
  uint8_t middle = spi_transaction(0x30, 0x00, 0x01, 0x00);
  Serial.print((char)middle);
  uint8_t low = spi_transaction(0x30, 0x00, 0x02, 0x00);
  Serial.print((char)low);
  Serial.print((char)STK_OK);
}
//////////////////////////////////////////
//////////////////////////////////////////

////////////////////////////////////
////////////////////////////////////
void avrisp() {
  uint8_t ch = getch();
  switch (ch) {
  case '0': // signon
    error = 0;
    empty_reply();
    break;
  case '1':
    if (getch() == CRC_EOP) {
      Serial.print((char)STK_INSYNC);
      Serial.print("AVR ISP");
      Serial.print((char)STK_OK);
    } else {
      error++;
      Serial.print((char)STK_NOSYNC);
    }
    break;
  case 'A':
    get_version(getch());
    break;
  case 'B':
    fill(20);
    set_parameters();
    empty_reply();
    break;
  case 'E': // extended parameters - ignore for now
    fill(5);
    empty_reply();
    break;
  case 'P':
    if (!pmode)
      start_pmode();
    empty_reply();
    break;
  case 'U': // set address (word)
    here = getch();
    here += 256 * getch();
    empty_reply();
    break;

  case 0x60: // STK_PROG_FLASH
    getch(); // low addr
    getch(); // high addr
    empty_reply();
    break;
  case 0x61: // STK_PROG_DATA
    getch(); // data
    empty_reply();
    break;

  case 0x64: // STK_PROG_PAGE
    program_page();
    break;

  case 0x74: // STK_READ_PAGE 't'
    read_page();
    break;

  case 'V': // 0x56
    universal();
    break;
  case 'Q': // 0x51
    error = 0;
    end_pmode();
    empty_reply();
    break;

  case 0x75: // STK_READ_SIGN 'u'
    read_signature();
    break;

  // expecting a command, not CRC_EOP
  // this is how we can get back in sync
  case CRC_EOP:
    error++;
    Serial.print((char)STK_NOSYNC);
    break;

  // anything else we will return STK_UNKNOWN
  default:
    error++;
    if (CRC_EOP == getch())
      Serial.print((char)STK_UNKNOWN);
    else
      Serial.print((char)STK_NOSYNC);
  }
}