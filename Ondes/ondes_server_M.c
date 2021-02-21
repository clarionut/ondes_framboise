/*
  ondes_server_M.c

  Provides an interface between the Ondes hardware and the PD session which
  generates the sound

  Hardware:
    Maya44 USB+ 4-channel USB sound card
    (previously was PiFi DAC+ 2-channel audio interface
       The PiFi card conflicts with the Raspberry Pi SPI1 interface
       so all three SPI devices must be placed on SPI0. This requires
       a custom device tree overlay to create an SPI0.2 port)
    
    mcp3008 SPI ADC to read ribbon, touche and control pots and pedals
       via IOCTL communication on SPI0.0

    mcp23s08 SPI port expander on SPI0.1 to scan the switches (including
       the vibrato switch on the keyboard) via IOCTL communication.
       GPIOs 19, 20 and 24 (for compatibility with the first prototype)
       are used to select the banks of switches while scanning

    adxl362 SPI accelerometer on non-standard SPI0.2 for vibrato control
       via IOCTL communication, using GPIO25 as CE2
       
    74hc595 shift registers (x2) to drive the red/green octave marker LEDs
       and the RGB LED in the Touche button. The octave markers are
       bidirectional LEDs connected to adjacent pairs of outputs on the
       74hc595s; 01 or 10 on these outputs give red or green, 00 or 11 OFF
       Uses GPIO ports 27, 22 & 23

    16x2 LCD with I2C backpack on GPIO ports 2 & 3, using the LCD1602
       library from GitHub - https://github.com/bitbank2/LCD1602

    Rotary encoder on GPIO 5 & 6, switch on GPIO 12 with connections
       defined using dtoverlay entries in /boot/config.txt

    MIDI keyboard scanned by this program. It's easier to keep track of
       multiple key-presses here rather than in PD, and we can keep the
       existing LIBLO messaging so the PD patch is unchanged

    Optional USB memory stick mounted on /usbdrive with subdirectories
       WAV and MIDI. These can be symbolically linked to /home/pi/Ondes/WAV
       and /home/pi/Ondes/MIDI to allow data files to be transferred easily.
       Without a USB stick use real directories instead of symbolic links and
       transfer data to and from the remote computer by scp.


  Modified:
    15 02 21 - branched from ondes_server.c to work with a MIDI keyboard
               rather than a switch-matrix keyboard

 cc -o ~/Ondes/ondes_server_M ondes_server_M.c -llo -lm -llcd1602 -I/usr/local/include
 
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <lo/lo.h>
#include <math.h>
#include <time.h>
#include <lcd1602.h>
#include <linux/ioctl.h>
#include <linux/input.h>
#include <linux/spi/spidev.h>

/* Defines for the 74hc595 lines & the 'extra' switch bank select
   using GPIO numbers */
#define SER    27
#define RCLK   22
#define SRCLK  23
#define SW_1   19
#define SW_2   20
#define SW_3   21

/* Defines for the MCP23S08 */
#define IODIR   0x00  // I/O direction
#define IPOL    0x01  // I/O polarity
#define GPINTEN 0x02  // interrupt enable
#define DEFVAL  0x03  // register default value (interrupts)
#define INTCON  0x04  // interrupt control
#define IOCON   0x05  // I/O config
#define GPPU    0x06  // port pullups
#define INTF    0x07  // interrupt flag (where the interrupt came from)
#define INTCAP  0x08  // interrupt capture (value at interrupt is saved here)
#define GPIO    0x09  // port
#define OLATA   0x0A  // output latch

/* MCP23S08 I/O config */
#define SEQOP_OFF   0x20 // incrementing address pointer
#define SEQOP_ON    0x00
#define DISSLW_ON   0x10 // slew rate
#define DISSLW_OFF  0x00
#define HAEN_ON     0x08 // hardware addressing
#define HAEN_OFF    0x00
#define ODR_ON      0x04 // open drain for interupts
#define ODR_OFF     0x00
#define INTPOL_HIGH 0x02 // interrupt polarity
#define INTPOL_LOW  0x00

#define WRITE_CMD 0
#define READ_CMD 1

/* Defines for the LCD display */
#define LCD_ADDR 0x27
#define LCD_DAT 1
#define LCD_CMD 0
#define LCD_BLINKON   1
#define LCD_CURSORON  2
#define LCD_DISPLAYON 4
#define LCD_LIGHTON   8

/* Defines for tiny_gpio functions */
#define GPSET0 7
#define GPSET1 8
#define GPCLR0 10
#define GPCLR1 11
#define GPLEV0 13
#define GPLEV1 14
#define GPPUD     37
#define GPPUDCLK0 38
#define GPPUDCLK1 39
#define PI_BANK (gpio>>5)
#define PI_BIT  (1<<(gpio&0x1F))
#define PI_INPUT  0
#define PI_OUTPUT 1
#define PI_ALT0   4
#define PI_ALT1   5
#define PI_ALT2   6
#define PI_ALT3   7
#define PI_ALT4   3
#define PI_PUD_OFF  0
#define PI_PUD_DOWN 1
#define PI_PUD_UP   2

uint8_t done    = 0;
uint8_t btn_d   = 0;
uint8_t rty_d   = 0;
uint8_t submenu = 0;
uint16_t analogueLast[8] = {9999, 9999, 9999, 9999, 9999, 9999, 9999, 9999};
uint8_t fsLast = 1;
uint8_t fs     = 1;
unsigned int analogueMillis;
unsigned int switchMillis;
unsigned int lcdMillis;
unsigned int loopMillis = 0;
uint8_t debug = 0;
uint8_t octUpPressed = 0;
uint8_t octDnPressed = 0;
uint8_t bothPressed  = 0;
int8_t  octaveShift  = 0;
int loopcount = 0;
static const uint8_t spi_mode   = 0;
static const uint8_t spi_bpw    = 8; // bits per word
static const uint32_t spi_speed = 3000000; // 3MHz - was 10
static const uint16_t spi_delay = 0;
static const char *spidev[3]    = { "/dev/spidev0.0",
				    "/dev/spidev0.1",
				    "/dev/spidev0.2" };
int mcp3008_fd, mcp23s08_fd, adxl632_fd, kb_fd;
uint8_t colour[8] = {0, 1, 3, 2, 6, 4, 5, 7};
uint8_t rgb_led   = 0;
uint8_t rgb_old   = 0;
uint16_t oct_led  = 0x0565; // 010101100101 => red, red, green, red, red, red
uint16_t ledMask  = 0xffff;
uint16_t recMask  = 0x0000;
uint8_t shiftreg_count = 0;

int16_t analogueVal[7];
uint8_t prevSws[3] = {0};
int     lastKey     = 60;
float   tuning      = 440.0;
int     vib;
uint8_t keyBits[16] = {0};
unsigned char inPacket[4];

lo_address pd_lo;

float palme_freq[][2] = { 69.3,   0.02,
			  73.42,  0.02,
			  77.78,  0.02,
			  82.41,  0.02,
			  87.31,  0.02,
			  92.5,   0.02,
			  98,     0.02,
			  103.83, 0.022,
			  110,    0.024,
			  116.54, 0.026,
			  123.47, 0.028,
			  130.81, 0.03,
			  138.59, 0.032,
			  146.83, 0.034,
			  155.56, 0.036,
			  164.81, 0.038,
			  174.61, 0.04,
			  184.99, 0.04,
			  195.99, 0.04,
			  207.65, 0.04,
			  220,    0.04,
			  233.08, 0.04,
			  246.94, 0.04,
			  261.62, 0.04,
			  277.18, 0.04,
			  293.66, 0.04,
			  311.12, 0.04,
			  329.62, 0.04,
			  349.22, 0.04,
			  369.99, 0.04,
			  391.99, 0.04,
			  415.3,  0.04,
			  440,    0.04,
			  466.16, 0.04,
			  493.88, 0.04,
			  523.25, 0.04,
			  554.37, 0.04,
			  587.33, 0.04,
			  622.25, 0.04,
			  659.25, 0.02,
			  698.46, 0.02,
			  739.99, 0.02,
			  783.99, 0.02,
			  830.61, 0.02,
			  880,    0.02,
			  932.33, 0.02,
			  987.77, 0.02,
			  1046.5, 0.02,
			  1108.7, 0.01,
			  1174.7, 0.01,
			  1244.5, 0.01,
			  1318.5, 0.01,
			  1396.9, 0.01,
			  1480,   0.01,
			  1568,   0.01,
			  1661.2, 0.01,
			  1760,   0.01,
			  1864.7, 0.01,
			  2093,   0.01
};

static volatile uint32_t  *gpioReg = MAP_FAILED;

/* tiny_gpio functions */
void gpioSetMode(uint8_t, uint8_t);
uint8_t gpioGetMode(uint8_t);
void gpioSetPullUpDown(uint8_t, uint8_t);
uint8_t gpioRead(uint8_t);
void gpioWrite(uint8_t, uint8_t);
void gpioTrigger(uint8_t, uint16_t, uint8_t);
uint32_t gpioReadBank1(void);
uint32_t gpioReadBank2(void);
void gpioClearBank1(uint32_t);
void gpioClearBank2(uint32_t);
void gpioSetBank1(uint32_t);
void gpioSetBank2(uint32_t);
int8_t gpioInitialise(void);

/* mcp23s08 functions */
static uint8_t get_spi_control_byte(uint8_t, uint8_t);
void mcp23s08_write_reg(uint8_t, uint8_t, uint8_t, int);
uint8_t mcp23s08_read_reg(uint8_t, uint8_t, int);

/* other miscellaneous functions */
void analogueReset(void);
uint32_t myMicros(void);
uint32_t myMillis(void);
void delay(uint32_t);
int spi_open(int);
int16_t read_mcp3008(uint8_t);
int8_t adxl632(uint8_t, uint8_t, uint8_t);
void srPulse(int);
void srSend(uint16_t);
void setOctaveLEDs(void);
void setToucheLED(void);
int  playMidiFile(void);
int  readVarLen(char **);
void parseEvent(char **);
void selectMidiFile(void);

/* Add the handlers to act on messages received from PD */
void liblo_error(int num, const char *m, const char *path);

int generic_handler(const char *path, const char *types, lo_arg **argv,
                    int argc, void *data, void *user_data);

int refresh_handler(const char *path, const char *types, lo_arg ** argv,
                    int argc, void *data, void *user_data);

int led_handler(const char *path, const char *types, lo_arg ** argv,
		int argc, void *data, void *user_data);

/* Functions for the rotary encoder */
void    getEncoderDescriptors(void);
uint8_t encoderPress(void);
int8_t  encoderRotate(void);

/* Declarations for the LCD / encoder */
uint8_t menuActive     = 0;
int8_t  menuItem       = 0;
char    menuText[][17] = {"Tuning  A ",
			  "Touche LED  ",
			  "Octave LED ",
			  "Record  ",
			  "Play MIDI No    ",
			  "Eject USB  No   ",
			  "Save config  No ",
			  "Update OS  No   ",
			  "Shutdown  No    "};
char    lcdText[17];
uint8_t lcdBacklight = 1;
uint8_t lcdBlink     = 0;
int8_t  maxMenu, clicks;
uint8_t doShutdown = 0;
uint8_t toucheLED    = 1;
uint8_t octaveLED    = 1;
uint8_t saveConfig   = 0;
uint8_t recording    = 0;
uint8_t doRecord     = 0;
uint8_t doUpdateOS   = 0;
uint8_t ejectUSB     = 0;

/* Declarations for MIDI playback */
uint8_t playMidi     = 0;
int ticksPerQtr;
unsigned int qtrMicros = 500000; // Set default MIDI tempo crotchet=120
uint8_t eoTrk[2];
int delta[2];
uint8_t track;
int octaveOffset;
uint8_t midiSws[3];
uint8_t claquement = 1; // Default to claquement mode for MIDI playback
uint8_t ruban      = 0; // Default to clavier mode
uint8_t midiCount  = 0;
int midiSel        = 0;
char **midiFile    = NULL;

int main(int argc, char *argv[]) {
  /* Check for command line arguments */
  for (uint8_t i = 1; i < argc; i++) {
    if (0 == strcasecmp(argv[i], "-debug")) debug = 1;
  }

  /* Set up the OSC stuff with a new server on port 4001
   * and add methods to handle the messages from PD */
  lo_server_thread st = lo_server_thread_new("4001", liblo_error);
  lo_address pd_addr  = lo_address_new(NULL, "4000");

  /* Method to match any path and args */
  lo_server_thread_add_method(st, NULL, NULL, generic_handler, NULL);

  /* add method that will match the path /refresh with no args */
  lo_server_thread_add_method(st, "/refresh", NULL, refresh_handler, NULL);

  /* Method for path /led with one int arg */
  lo_server_thread_add_method(st, "/led", "i", led_handler, NULL);

  lo_server_thread_start(st);

  /* Create an address for communication with PD's OSC server */
  pd_lo = lo_address_new(NULL, "4000");

  /* Set up the hardware interfaces */
  /* The MCP3008 connection is on SPI0.0 */
  mcp3008_fd = spi_open(0);
  
  /* Set up the mcp23s08 on SPI0.1 and configure its ports */
  mcp23s08_fd = spi_open(1);
  const uint8_t ioconfig = SEQOP_OFF | DISSLW_OFF | HAEN_OFF | ODR_OFF | INTPOL_LOW;
  mcp23s08_write_reg(ioconfig, IOCON, 0, mcp23s08_fd);
  mcp23s08_write_reg(0xFF, IODIR, 0, mcp23s08_fd); // all pins are inputs
  mcp23s08_write_reg(0xFF, GPPU,  0, mcp23s08_fd); // enable pullups

  /* Initialise tiny_gpio, set GPIO19, 20, 21, 22, 23 & 27 as outputs
     and initialise the octave and touche LEDs*/
  gpioInitialise();
  gpioSetMode(SW_1, PI_OUTPUT);
  gpioWrite(SW_1, 1);
  gpioSetMode(SW_2, PI_OUTPUT);
  gpioWrite(SW_2, 1);
  gpioSetMode(SW_3, PI_OUTPUT);
  gpioWrite(SW_3, 1);
  gpioSetMode(SER, PI_OUTPUT);
  gpioWrite(SER, 0);
  gpioSetMode(RCLK, PI_OUTPUT);
  gpioWrite(RCLK, 0);
  gpioSetMode(SRCLK, PI_OUTPUT);
  gpioWrite(SRCLK, 0);
  srSend(0x0000); // all LEDs off

  /* The ADXL632 connection is on the non-standard SPI0.2 */
  adxl632_fd = spi_open(2);
  adxl632(0x0A, 0x1F, 0x52); // ADXL632 soft reset
  delay(1);
  adxl632(0x0A, 0x2D, 0x02); // ADXL632 enable measurement

  /* Get a file descriptor for the MIDI keyboard */
  kb_fd = open("/dev/snd/midiC1D0", O_RDONLY | O_NONBLOCK);
  if (-1 == kb_fd) {
    fprintf(stderr, "Error: cannot open /dev/snd/midiC1D0\n");
  }
  
  /* Start the PD process */
  system("pd -nogui /home/pi/Ondes/PD/Ondes.pd &");

  /* Set up the rotary encoder */
  getEncoderDescriptors();

  /* Read the config file if it exists */
  { FILE *cf_d;
    char line[80];
    char *offset;
    int pos;
    if ((cf_d = fopen("/home/pi/.ondesconfig", "r"))) {
      while (fgets(line, 80, cf_d)) {
	//fprintf(stderr, "%s", line);
	if (0 == strncmp(line, "touche ", 7)) {
	  offset = &line[7];
	  toucheLED = atoi(offset);
	  if (toucheLED > 1) toucheLED = 1;
	  setToucheLED();
	} else if (0 == strncmp(line, "octave ", 7)) {
	  offset = &line[7];
	  octaveLED = atoi(offset);
	  if (octaveLED > 2) octaveLED = 1;
	  setOctaveLEDs();
	} else if (0 == strncmp(line, "tuning ", 7)) {
	  offset = &line[7];
	  tuning = atof(offset);
	}
      }
      fclose(cf_d);
    }
  }

  /* Set up the LCD display */
  lcd1602Init(1, LCD_ADDR);
  lcd1602Control(1, 0, 0); // backlight, nocursor, noblink
  lcd1602WriteString("Ondes  Framboise");
  lcd1602SetCursor(0, 1);
  sprintf(lcdText, "%s%5.1f ", menuText[0], tuning);
  lcd1602WriteString(lcdText);
  maxMenu = sizeof(menuText) / 17;

  /* Turn on the LEDs if needed */
  srSend(oct_led);
  
  analogueReset();
  analogueMillis = myMillis();
  switchMillis   = analogueMillis;
  loopMillis     = analogueMillis;
  lcdMillis      = analogueMillis;

  /* The main processing loop */
  while (!done) {
    /* Read the analogue values and send them to PD if they've changed
       Values are:
       0 - Touche
       1 - Ruban
       2 - Octaviant level
       3 - Petit gambe level
       4 - Souffle level
       5 - Effects speaker(s) volume
       6 - Expression pedal volume
       7 - Feutre pedal cutoff
     */
    ++loopcount;
    /* Analogue values */
    if ((myMillis() - analogueMillis) >= 5) {
      uint8_t changed = 0;
      for (uint8_t i = 0; i < 8; i++) {
	analogueVal[i] = read_mcp3008(i);
      }
      /* Set the range for the Touche control (do it here to
	 avoid sending unnecessary UDP messages) */
      if (analogueVal[0] > 920) analogueVal[0] = 920;
      if (analogueVal[0] < 100) analogueVal[0] = 100;
      analogueVal[0] = 920 - analogueVal[0];
      for (uint8_t i = 0; i < 8; i++) {
	/* Filter noise in the lowest bits from the A/D conversion
	   '> 0' in the line below means no filter */
	if (abs(analogueVal[i] - analogueLast[i]) > 1) {
	  analogueLast[i] = analogueVal[i];
	  changed = 1;
	}
      }
      if (changed) {
	lo_send(pd_lo, "/anlg", "iiiiiiii",
		analogueVal[0], analogueVal[1], analogueVal[2],
		analogueVal[3], analogueVal[4], analogueVal[5],
		analogueVal[6], analogueVal[7]);
      }

      /* Read the accelerometer 8-bit X-axis for vibrato */
      vib = adxl632(0x0B, 0x08, 0x00);
      lo_send(pd_lo, "/vib", "i", vib);

      analogueMillis += 5;
    }

    /* Scan the physical switches into the switches[] array:
       01 - 07 addressed by GPIO19 (there is no switch in the first position)
       08 - 15 addressed by GPIO20
       16 - 23 addressed by GPIO21  */
    if ((myMillis() - switchMillis) >= 15) { // was 10
      uint8_t switches[3] = {0};
      uint8_t changed = 0;
      uint8_t gpio[] = {SW_1, SW_2, SW_3};
      for (uint8_t i = 0; i <= 2; i++) {
	/* pull the rows of the switch matrix low in turn */
	gpioWrite(gpio[i], 0);
	switches[i] = mcp23s08_read_reg(GPIO, 0, mcp23s08_fd);
	/* Invert here so ON switches = 1, OFF = 0 before sending to PD */
	switches[i] = ~switches[i];
	
	if (switches[i] != prevSws[i]) {
	  changed = 1;
	  prevSws[i] = switches[i];
	}
	gpioWrite(gpio[i], 1);
      }

      if (changed) {
	/* The switches have changed so send the current setting */
	if (debug) fprintf(stderr,
	    "Switches: %2.2x %2.2x %2.2x\n",
	    switches[2], switches[1], switches[0]);
	changed = 0;

	/* Send the switches.
	   switches[0] bits:
	   0   - N/C
	   1   - keyboard mounted vibrato switch.
           2   - 'T' switch - turn on all voices except Souffle
	                      (switches[1] bit 1) if this is set
	   3-7 - Ondes, Creux, Gambe, Nasillard & Octaviant

	   switches[1] bits:
	   0-1 - petit Gambe & Souffle
	   2   - Clavier / Ruban selection
	   3   - legato / claquement keyboard mode
	   4-7 - D1 - D4 selectors

	   switches[2] bits:
	   0-5 - transposition buttons
	   6-7 - octave shifters */
	if (switches[0] & 4) {
	  /* turn on voices if 'T' is on */
	  switches[0] |= 248; // Ondes, Creux, Gambe, Nasillard, Octaviant
	  switches[1] |= 1;   // petit Gambe
	}
	/* Bits 6 & 7 of switches[8] are the octave shifters
	   - mask these when sending switch data to PD */
	lo_send(pd_lo, "/sw", "iii", switches[0], switches[1], switches[2] & 63);

	/* Check the octave shift buttons */
	if (switches[2] & 64) {
	  /* Octave down pressed */
	  if (!octDnPressed) {
	    /* It wasn't pressed last pass, so update and send the octave */
	    if (octaveShift > -24) octaveShift -= 12;
	    octDnPressed = 1;
	    lo_send(pd_lo, "/oct", "i", octaveShift);
	    if (debug) fprintf(stderr, "Octave shift down %d\n", octaveShift);
	    /* Change the octave marker LEDs by flipping the pair of bits
	       corresponding to the selected octave.
	       Note: 0x0555 is binary 010101010101 - 6 red LEDs */
	    oct_led = 0x0555 ^ (3 << 4 + (octaveShift / 6));
	    srSend(((colour[rgb_led] << 12) | oct_led) & ledMask);
	    shiftreg_count = 0;
	  }
	} else {
	  octDnPressed = 0;
	}
	if (switches[2] & 128) {
	  /* Octave up pressed */
	  if (!octUpPressed) {
	    /* It wasn't pressed last pass, so update and send the octave */
	    if (octaveShift < 12) octaveShift += 12;
	    octUpPressed = 1;
	    lo_send(pd_lo, "/oct", "i", octaveShift);
	    if (debug) fprintf(stderr, "Octave shift up %d\n", octaveShift);
	    /* Change the octave marker LEDs by flipping the pair of bits
	       corresponding to the selected octave.
	       Note: 0x0555 is binary 010101010101 - all LEDs red */
	    oct_led = 0x0555 ^ (3 << 4 + (octaveShift / 6));
	    srSend(((colour[rgb_led] << 12) | oct_led) & ledMask);
	    shiftreg_count = 0;
	  }
	} else {
	  octUpPressed = 0;
	}
      } /* end of 'if (changed)' */

      /* The Ondes has low-note priority, so scan UP the keyboard.
         If we find a pressed key send its code if it's different
	 from last time and stop scanning.
	 In legato mode send nothing if no keys are pressed,
	 in claquement mode send 'play 0' message when the
	 last key is released. */
      inPacket[0] = 0; // remove previous data
      read(kb_fd, &inPacket, sizeof(inPacket));
      if ((144 == inPacket[0]) || (128 == inPacket[0])) {
	if (144 == inPacket[0]) {
	  /* It's a note-on event */
	  keyBits[inPacket[1] / 8] |= (1 << (inPacket[1] % 8));
	} else if (128 == inPacket[0]) {
	  /* It's a note-off event */
	  keyBits[inPacket[1] / 8] &= ~(1 << (inPacket[1] % 8));
	}
	/* something has changed so scan up the keyBits array, find
	   the lowest note and send a message to PD if it's changed */
	uint8_t lowest = 255;
	for (uint8_t i = 0; (i < 16) && (255 == lowest); i++) {
	  uint8_t keyMask = 1;
	  for (uint8_t j = 0; (j < 8) && (255 == lowest); j++) {
	    if (keyBits[i] & keyMask) {
	      lowest = i * 8 + j;
	    }
	    keyMask <<= 1;
	  }
	}
	if ((255 == lowest) && (switches[1] & 8)) {
	  /* All keys released - send play=0 if claquement mode */
	  lo_send(pd_lo, "/key", "ii", lastKey - 36, 0);
	} else if (255 != lowest) {
	  /* Send the lowest 'real' note to PD (255 => no key pressed) */
	  lastKey = lowest;
	  lo_send(pd_lo, "/key", "ii", lastKey - 36, 1);
	}
      }
      
      switchMillis += 15;
    }
    
    ++ shiftreg_count;
    if (50 == shiftreg_count) {
      /* refresh the shift registers 'just in case' */
      srSend(((colour[rgb_led] << 12) | oct_led) & ledMask);
      shiftreg_count = 0;
    }
    usleep(1000);

    /* Check for and process rotary encoder activity */
    if (encoderPress()) {
      /* Encoder button pressed
	 If 'idle' (LED off) turn on the LED but do nothing else */
      lcdMillis = myMillis();
      if (0 == lcdBacklight) {
	lcdBacklight = 1;
	lcd1602Control(lcdBacklight, 0, menuActive);

      } else if (!menuActive) {
	/* Select the current menu item (blink cursor) */
	menuActive = 1;
	switch (menuItem) {
	case 0: // Tuning
	case 8: // Shutdown
	  lcd1602SetCursor(9, 1);
	  break;
	case 1: // Touche LED
	  lcd1602SetCursor(11, 1);
	  break;
	case 2: // Octave LEDs
	  lcd1602SetCursor(10, 1);
	  break;
	case 3: // Record to WAV
	  lcd1602SetCursor(7, 1);
	  break;
	case 4: // Select / play MIDI file
	  lcd1602SetCursor(9, 1);
	  break;
	case 5: // Eject USB drive
	  lcd1602SetCursor(10, 1);
	  break;
	case 6: // Save config
	  lcd1602SetCursor(12, 1);
	  break;
	case 7: // Update OS
	  lcd1602SetCursor(10, 1);
	  break;
	}
	lcdBlink = 1;
	lcd1602Control(lcdBacklight, 0, menuActive);

      } else {
	/* Process the selected menu item */
	menuActive = 0;
	lcd1602Control(lcdBacklight, 0, menuActive); // BLINK OFF
	switch (menuItem) {
	case 0: // Tuning
	  break;
	case 1: // Touche LED
	  setToucheLED();
	  break;
	case 2: // Octave LEDs
	  setOctaveLEDs();
	  break;
	case 3: // Record to WAV
	  if (doRecord) {
	    lcd1602SetCursor(0, 0);
	    doRecord = 0;
	    if (recording) {
	      //fprintf(stderr, "Was recording, now stopped\n");
	      lo_send(pd_lo, "/record", "s", "stop");
	      lcd1602WriteString("Ondes  Framboise");
	      lcd1602SetCursor(8, 1);
	      lcd1602WriteString("No      ");
	      recording = 0;
	      recMask = 0x0000;
	    } else {
	      //fprintf(stderr, "Wasn't recording, now started\n");
	      lcd1602WriteString("Recording  >>>  ");
	      lcd1602SetCursor(8, 1);
	      lcd1602WriteString("Stop    ");
	      recording = 1;
	      doRecord = 1;
	      recMask = 0x0fff;
	      char wavName[13];
	      time_t t = time(NULL);
	      struct tm *tm = localtime(&t);
	      sprintf(wavName, "%2.2d%2.2d%2.2d%2.2d%2.2d%2.2d",
		      tm->tm_year - 100, tm->tm_mon + 1, tm->tm_mday,
		      tm->tm_hour, tm->tm_min, tm->tm_sec);
	      //fprintf(stderr, "%s\n", wavName);
	      lo_send(pd_lo, "/record", "s", wavName);
	    }
	  }
	  break;
	case 4: // Select or play MIDI file
	  if (1 == playMidi) {
	    /* Play the selected MIDI file */
	    lcd1602SetCursor(10, 1);
	    lcd1602WriteString(" >>>");
	    lcd1602SetCursor(10, 1);
	    playMidiFile();
	    lcd1602WriteString("Done");
	    playMidi = 0;
	    /* Stop recording if active at the end of MIDI playback */
	    if (recording) {
	      lo_send(pd_lo, "/record", "s", "stop");
	      recording = 0;
	      doRecord = 0;
	      lcd1602SetCursor(0, 0);
	      lcd1602WriteString("Ondes  Framboise");
	      recMask = 0x0000;
	    }
	  } else if (2 == playMidi) {
	    /* Select a MIDI file */
	    selectMidiFile();
	  }
	  break;
	case 5: // Eject USB
	  if (ejectUSB) {
	    lcd1602SetCursor(11, 1);
	    lcd1602WriteString(">>>>");
	    system("sudo umount /usbdrive");
	    lcd1602SetCursor(11, 1);
	    lcd1602WriteString("Done");
	    ejectUSB = 0;
	    lcdMillis = myMillis();
	  }
	  break;
	case 6: // Save config
	  if (saveConfig) {
	    FILE *cf_d;
	    int fail = 1;
	    if ((cf_d = fopen("/home/pi/.ondesconfig", "w"))) {
	      fprintf(cf_d, "tuning %5.1f\ntouche %1.1d\noctave %1.1d\n",
		      tuning, toucheLED, octaveLED);
	      fail = fclose(cf_d);
	    }
	    lcd1602SetCursor(13, 1);
	    lcd1602WriteString((fail) ? "XXX" : "OK ");
	    saveConfig = 0;
	  }
	  break;
	case 7: // Update OS
	  if (doUpdateOS) {
	    lcd1602SetCursor(11, 1);
	    lcd1602WriteString(">>>>");
	    system("sudo apt-get update && sudo apt-get -y dist-upgrade");
	    lcd1602SetCursor(11, 1);
	    lcd1602WriteString("Done");
	    doUpdateOS = 0;
	    lcdMillis = myMillis();
	  }
	  break;
	case 8: // Shutdown
	  if (1 == doShutdown) {
	    lcd1602SetCursor(0, 1);
	    lcd1602WriteString("  Restarting!   ");
	    done = 1;
	  } else if (2 == doShutdown) {
	    lcd1602SetCursor(0, 1);
	    lcd1602WriteString(" Shutting down! ");
	    done = 1;
	  }
	}
      }
    }

    if ((clicks = encoderRotate())) {
      /* The encoder has been turned
	 If 'idle' (LED off) turn on the LED but do nothing else */
      lcdMillis = myMillis();
      if (0 == lcdBacklight) {
	lcdBacklight = 1;
	lcd1602Control(lcdBacklight, 0, menuActive);

      } else if (menuActive) {
	/* Process the actions for the selected menu item */
	switch (menuItem) {
	case 0: // Tuning
	  tuning += (float) clicks * 0.1;
	  lcd1602SetCursor(10, 1);
	  sprintf(lcdText, "%5.1f ", tuning);
	  lcd1602WriteString(lcdText);
	  lcd1602SetCursor(9, 1);
	  lo_send(pd_lo, "/tuning", "f", tuning);
	  break;
	case 1: // touche LED
	  toucheLED = !toucheLED;
	  lcd1602SetCursor(12, 1);
	  lcd1602WriteString((toucheLED) ? "On  " : "Off ");
	  lcd1602SetCursor(11, 1);
	  break;
	case 2: // octave LED
	  octaveLED += clicks / abs(clicks);
	  while (octaveLED < 0) octaveLED += 4;
	  octaveLED %= 4;
	  lcd1602SetCursor(11, 1);
	  switch (octaveLED) {
	  case 0:
	    lcd1602WriteString("Off  ");
	    break;
	  case 1:
	    lcd1602WriteString("All  ");
	    break;
	  case 2:
	    lcd1602WriteString("Mid C");
	    break;
	  case 3:
	    lcd1602WriteString("Shift");
	    break;
	  }
	  lcd1602SetCursor(10, 1);
	  break;
	case 3: // Record to WAV
	  doRecord = !doRecord;
	  lcd1602SetCursor(8, 1);
	  if (doRecord) {
	    lcd1602WriteString((recording) ? "No     " : "Start  ");
	  } else {
	    lcd1602WriteString((recording) ? "Stop   " : "No     ");
	  }	    
	  lcd1602SetCursor(7, 1);
	  break;
	case 4: // Play MIDI file
	  playMidi += 3 + clicks / abs(clicks);
	  playMidi %= 3;
	  if (!midiSel && (1 == playMidi)) playMidi += clicks / abs(clicks);
	  //fprintf(stderr, "playMidi: %d   midiSel: %d\n", playMidi, midiSel);
	  lcd1602SetCursor(10, 1);
	  switch (playMidi) {
	  case 0:
	    lcd1602WriteString("No    ");
	    break;
	  case 1:
	    lcd1602WriteString("Play  ");
	    break;
	  case 2:
	    lcd1602WriteString("Select");
	    break;
	  }
	  lcd1602SetCursor(9, 1);
	  break;
	case 5: // Eject USB
	  ejectUSB = !ejectUSB;
	  lcd1602SetCursor(11, 1);
	  lcd1602WriteString((ejectUSB) ? "Yes " : "No  ");
	  lcd1602SetCursor(10, 1);
	  break;
	case 6: // Save config
	  saveConfig = !saveConfig;
	  lcd1602SetCursor(13, 1);
	  lcd1602WriteString((saveConfig) ? "Yes" : "No ");
	  lcd1602SetCursor(12, 1);
	  break;
	case 7: // Update OS
	  doUpdateOS = !doUpdateOS;
	  lcd1602SetCursor(11, 1);
	  lcd1602WriteString((doUpdateOS) ? "Yes " : "No  ");
	  lcd1602SetCursor(10, 1);
	  break;
	case 8: // Shutdown
	  doShutdown += 3 + clicks / abs(clicks);
	  doShutdown %= 3;
	  lcd1602SetCursor(10, 1);
	  switch(doShutdown) {
	  case 0:
	    lcd1602WriteString("No    ");
	    break;
	  case 1:
	    lcd1602WriteString("Reboot");
	    break;
	  case 2:
	    lcd1602WriteString("Halt  ");
	    break;
	  }
	  lcd1602SetCursor(9, 1);
	  break;
	}

      } else {
	/* Scroll through the menu items - enforce one at a time */
	menuItem += clicks / abs(clicks);
	while (menuItem < 0) menuItem += maxMenu; // keep the result in
	menuItem %= maxMenu;                      // range 0 < maxMenu
	lcd1602SetCursor(0, 1);
	switch (menuItem) {
	case 0: // Tuning
	  sprintf(lcdText, "%s%5.1f ", menuText[menuItem], tuning);
	  lcd1602WriteString(lcdText);
	  break;
	case 1: // Touche LED
	  lcd1602WriteString(menuText[menuItem]);
	  lcd1602WriteString((toucheLED)?"On  ":"Off ");
	  break;
	case 2: // Octave LEDs
	  lcd1602WriteString(menuText[menuItem]);
	  switch (octaveLED) {
	  case 0:
	    lcd1602WriteString("Off  ");
	    break;
	  case 1:
	    lcd1602WriteString("All  ");
	    break;
	  case 2:
	    lcd1602WriteString("Mid C");
	    break;
	  case 3:
	    lcd1602WriteString("Shift");
	    break;
	  }
	  break;
	case 3: // Record to WAV
	  lcd1602WriteString(menuText[menuItem]);
	  lcd1602WriteString((recording)?"Stop    ":"No      ");
	  break;
	case 4: // Play MIDI file
	case 5: // Eject USB
	case 6: // Save config
	case 7: // Update OS
	case 8: // Shutdown
	  lcd1602WriteString(menuText[menuItem]);
	  break;
	}
	lcd1602Control(lcdBacklight, 0, menuActive);
      }
    } /* End of 'if ((clicks = encoderRotate()))' */

    if (lcdBacklight && ((myMillis() - lcdMillis) > 20000)) {
      /* Turn off the backlight if encoder idle for 20s */
      lcdBacklight = 0;
      lcd1602Control(lcdBacklight, 0, menuActive);
    }
  } /* End of main 'while (!done)' loop */

  /* Shutdown - send a quit message to PD, set the 'outer' octave LEDs
     to off, middle C and Touche red, close OSC, then shut down
     the Raspberry Pi */
  lo_send(pd_lo, "/quitpd", "i", 1);
  delay(1000);
  lo_server_thread_free(st);
  lcd1602SetCursor(0, 1);
  if (1 == doShutdown) {
    /* Set touche and middle C marker green */
    srSend(0x2020); // was 0x0820); // was 0x1C10
    system("sudo shutdown -r now");
  } else {
    /* Set touche and middle C marker red */
    srSend(0x1010); // was 0x0410); // was 0x1C10
    system("sudo shutdown -h now");
  }
  
  return 0;
}


void liblo_error(int num, const char *msg, const char *path) {
  fprintf(stderr, "liblo server error %d in path %s: %s\n", num, path, msg);
}

int generic_handler(const char *path, const char *types, lo_arg **argv,
                    int argc, void *data, void *user_data) {
  /* Catch any incoming messages which are not dealt with by other handlers.
     returning 1 means that the message has not been fully handled and the
     server should try other methods */
  int i;
  if (strncmp(path, "/oled/line", 9) &&
      strncmp(path, "/quit", 5) &&
      strncmp(path, "/refresh", 8) &&
      strncmp(path, "/led", 4)) {
    printf("Message: path <%s>, argc <%d>\n", path, argc);
    for (i = 0; i < argc; i++) {
      fprintf(stderr, "arg %d '%c' ", i, types[i]);
      lo_arg_pp((lo_type)types[i], argv[i]);
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
  }

  return 1;
}

int refresh_handler(const char *path, const char *types, lo_arg **argv,
                 int argc, void *data, void *user_data) {
  lo_send(pd_lo, "/tuning", "f", tuning);
  lo_send(pd_lo, "/key", "ii", 24, 0); // Set middle C as active note
  lo_send(pd_lo, "/anlg", "iiiiiiii",
	  analogueVal[0], analogueVal[1], analogueVal[2],
	  analogueVal[3], analogueVal[4], analogueVal[5],
	  analogueVal[6], analogueVal[7]);
  lo_send(pd_lo, "/vib", "i", vib);
  lo_send(pd_lo, "/oct", "i", octaveShift);
  lo_send(pd_lo, "/sw", "iii", prevSws[0] & 254, prevSws[1], prevSws[2] & 63);

  return 0;
}

int led_handler(const char *path, const char *types, lo_arg **argv,
                 int argc, void *data, void *user_data) {
  rgb_led = argv[0]->i;
  srSend(((colour[rgb_led] << 12) | oct_led) & ledMask);
  shiftreg_count = 0;
  //fprintf(stderr, "LED colour %d\n", rgb_led);

  return 0;
}

void analogueReset(void) {
  for (uint8_t i = 0; i < 6; i++) {
    analogueLast[i] = 10000;
  }
}

uint32_t myMillis(void) {
  /* get the number of milliseconds since the arbitrary start time */
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC_RAW, &tm);

  return (unsigned int) round(tm.tv_nsec / 1.0e6) + tm.tv_sec * 1000;
}

uint32_t myMicros(void) {
  /* Get the number of microseconds since the arbitrary start time */
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC_RAW, &tm);

  return (uint32_t) ((uint64_t)((tm.tv_nsec + 500) / 1000) + (uint64_t)tm.tv_sec * (uint64_t)1.0e6);
}

void delay(unsigned int millis) {
  // Wait for specified number of milliseconds 
  struct timespec sleep;
  sleep.tv_sec - (time_t) (millis / 1000);
  sleep.tv_nsec = (uint64_t) (millis % 1000) * 1000000;
  nanosleep(&sleep, NULL);
}

int spi_open(int chip_select) {
  /* Open the specified SPI device */
  int fd;
  if ((fd = open(spidev[chip_select], O_RDWR)) < 0) {
    fprintf(stderr, "spi_open: ERROR Could not open SPI device (%s).\n",
	    spidev[chip_select]);
    return -1;
  }

  /* Initialise the SPI device */
  if (ioctl(fd, SPI_IOC_WR_MODE, &spi_mode) < 0) {
    fprintf(stderr, "spi_open: ERROR Could not set SPI mode.\n");
    close(fd);
    return -1;
  }
  if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bpw) < 0) {
    fprintf(stderr,
	    "spi_open: ERROR Could not set SPI bits per word.\n");
    close(fd);
    return -1;
  }
  if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0) {
    fprintf(stderr, "spi_open: ERROR Could not set SPI speed.\n");
    close(fd);
    return -1;
  }

  return fd;
}

int16_t read_mcp3008(uint8_t channel) {
  /* Read the specified channel of the ADC */
  uint8_t buf[] = {0x01, 0x80, 0x00};
  buf[1] |= (channel << 4);

  struct spi_ioc_transfer spi;
  memset (&spi, 0, sizeof(spi));
  spi.tx_buf = (unsigned int) buf;
  spi.rx_buf = (unsigned int) buf;
  spi.len = 3;
  spi.delay_usecs = 0;
  spi.speed_hz = 3000000;
  spi.bits_per_word = 8;
  
  /* do the SPI transaction */
  if ((ioctl(mcp3008_fd, SPI_IOC_MESSAGE(1), &spi) < 0)) {
    fprintf(stderr, "mcp3008: There was an error during the SPI transaction.\n");
  }

  /* Assemble the 10-bit result for return.
     The two MSBs are in byte 1, the 8 LSBs in byte 2 */
  int16_t retval = (int16_t) (buf[1] & 3);
  retval <<= 8;
  retval += buf[2];

  return retval;
}

int8_t adxl632(uint8_t b0, uint8_t b1, uint8_t b2) {
  /* Communicate with the accelerometer */
  uint8_t buf[] = {b0, b1, b2};
  struct spi_ioc_transfer spi;
  memset (&spi, 0, sizeof(spi));
  spi.tx_buf = (unsigned int) buf;
  spi.rx_buf = (unsigned int) buf;
  spi.len = 3;
  spi.delay_usecs = 0;
  spi.speed_hz = 3000000;
  spi.bits_per_word = 8;

  /* do the SPI transaction */
  if ((ioctl(adxl632_fd, SPI_IOC_MESSAGE(1), &spi) < 0)) {
    fprintf(stderr, "ADXL632: There was an error during the SPI transaction.\n");
  }

  //fprintf(stderr, "ADXL362 Device ID: %02X %02X %02X\n", buf[0], buf[1], buf[2]);

  return (int8_t) buf[2];
}

void srPulse(int pin) {
  gpioWrite(pin, 0);
  gpioWrite(pin, 1);
}

void srSend(uint16_t data) {
  /* We don't need to use the final 3 bits of the 2nd 74hc595.
     For all 16 bits mask would start at 0x8000 and the loop limit
     would be 16 rather than 13
     N.B. the bits are inverted when written out to the 74HC595s,
     so an 'on' bit in the input data corresponds to an 'on' LED */
  uint16_t mask = 0x4000;
  data ^= recMask; // inverts the Octave LED colours when recording
  for (uint8_t i = 0; i < 15; i++) {
    gpioWrite(SER, ((data & mask) == 0));
    srPulse(SRCLK);
    mask >>= 1;
  }
  srPulse(RCLK);
}

void getEncoderDescriptors(void) {
  /* Get the descriptors for the rotary encoder and its button */
  char eventName[256];
  char devName[20];
  
  for (uint8_t i = 0; i < 10; i++) {
    sprintf(devName, "/dev/input/event%u", i);
    int fd = open(devName, O_RDONLY | O_NONBLOCK);
    if (-1 != fd) {
      ioctl(fd, EVIOCGNAME(sizeof(eventName)), eventName);
      if (!strncmp(eventName, "button", 6)) {
        btn_d = fd;
      } else if (!strncmp(eventName, "rotary", 6)) {
        rty_d = fd;
      } else {
        close(fd);
      }
    }
    /* Stop looking once we've got both encoder and button */
    if (btn_d && rty_d) break;
  }
}

uint8_t encoderPress(void) {
  struct input_event ev[64];
  int rd = read(btn_d, ev, sizeof(struct input_event) * 64);
  if (rd > 0) {
    for (uint8_t j = 0; j < rd / sizeof(struct input_event); j++) {
      if (ev[j].value) return 1;
    }
  }
  return 0;
}

int8_t encoderRotate(void) {
  struct input_event ev[64];
  int rd = read(rty_d, ev, sizeof(struct input_event) * 64);
  int clicks = 0;
  if (rd > 0) {
    for (uint8_t j = 0; j < rd / sizeof(struct input_event); j++) {
      /* given current wiring of the encoder, reverse the direction
         so positive means clockwise */
      clicks -= ev[j].value;
    }
  }
  return clicks;
}

void setOctaveLEDs(void) {
  /* various options for the ribbon octave markers, now updated for 6 LEDs */
  switch (octaveLED) {
  case 0: // Off
    ledMask &= 0xf000; // was 0x3c00 for 5 octave LEDs;
    break;
  case 1: // All
    ledMask |= 0x0fff; // was 0x03ff;
    break;
  case 2: // middle C
    ledMask |= 0x0fff; // was 0x03ff; 
    ledMask &= 0xfaaa; // was 0x3eaa;
    break;
  case 3: // middle C, only when shifted
    ledMask |= 0x0fff; // was 0x03ff; 
    ledMask &= 0xfa8a; // was 0x3e8a;
    break;
  }
}

void setToucheLED(void) {
  if (toucheLED) {
    ledMask |= 0x3c00;
  } else {
    ledMask &= 0x03ff;
  }
}

int playMidiFile(void) {
  /* Subroutine to play a stored MIDI file. Takes over control from the
     main loop and passes messages to Pure Data based on values from
     the MIDI file rather than by reading the ADC and port expander.

     Voices are selected by Program Change events, with individual bits
     corresponding to the different voices of the Ondes:
     1 - Ondes, 2 - Creux, 4 - Gambe, 8 - Nasillard, 16 - Octaviant
     32 - petit gambe, 64 - Souffle

     Volume (sent as expression pedal values) using Expression MSB

     Ruban pitch and vibrato notated as Pitch Bend events in MIDI file,
     sent as new /ruban endpoint for Ruban and as /vib values for vibrato.
     Ruban range goes from C-1 (MIDI 0) at 0 to (nearly) C7 (MIDI 109)
     at 16383, with middle C at 8192.

     General Purpose Controllers 1-3 (16-18; 0x10-0x12) control the
     subsidiary voice level values:
     1 - octaviant, 2 - petit gambe, 3 - Souffle

     GPC 4 (19; 0x13) for the D2-D4 relative level control

     GPC 5 (80; 0x50) bitwise Diffuseur selection:
     1 - D1, 2 - D2, 4 - D3, 8 - D4

     GPC 6 (81; 0x51) for Clavier (<=63) or Ruban (>=64)

     GPC 7 (82; 0x52) for Legato (<=63) or Claquement (>=64) selection.
     Note-off events ignored in legato mode, send 'play = 0' in the /key
     message in claquement mode.

     GPC 8 (83; 0x53) for analogue Feutre value */

  
  /* Open the MIDI file memory mapped so we can do pointer operations */
  char *trk_p[2], *tmp_p;
  uint8_t format, numTrks;
  int trkLen[2] = { 0, 0 };
  struct stat sb;

  qtrMicros = 500000; // Set default MIDI tempo crotchet=120
  eoTrk[0] = 0;
  eoTrk[1] = 0;
  delta[0] = 0;
  delta[1] = 0;
  midiSws[0] = prevSws[0]; // Initialise with the current Tiroir settings
  midiSws[1] = prevSws[1];
  midiSws[2] = prevSws[2];

  char tmpName[60];
  sprintf(tmpName, "/usbdrive/MIDI/%s", midiFile[midiSel]);
  int midi_d = open(tmpName, O_RDONLY);
  fstat(midi_d, &sb);
  char *midi_p = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, midi_d, 0);

  /* Read the MThd info */
  tmp_p = memmem(midi_p, 200, "MThd", 4);
  if (NULL == tmp_p) {
    fprintf(stderr, "Failed to find MThd record\n");
    return -1;
  }
  tmp_p += 9;
  format = tmp_p[0];
  numTrks = tmp_p[2];
  ticksPerQtr = tmp_p[3] << 8;
  ticksPerQtr |= tmp_p[4];

  /* Find the tracks and their lengths (which may not be needed) */
  trk_p[0] =  midi_p;
  for (track = 0; track < 2; track++) {
    tmp_p = trk_p[0];
    trk_p[track] = memmem(tmp_p, sb.st_size - 20, "MTrk", 4);
    if (NULL == trk_p[track]) {
      fprintf(stderr, "Failed to find MTrk record for track %d\n", track);
      return -1;
    }
    trk_p[track] += 4;
    for (uint8_t i = 0; i < 4; i++) {
      trkLen[track] <<= 8;
      trkLen[track] |= trk_p[track][0];
      ++trk_p[track]; 
    }
    /* Read the first delta time for each track leaving the pointers
       pointing to the first events */
    delta[track] = readVarLen(&trk_p[track]);
  }

  //fprintf(stderr, "Format: %d   Tracks: %d   Ticks/Qtr: %d  Trk1Len: %d  Trk2Len: %d\n", format, numTrks, ticksPerQtr, trkLen[0], trkLen[1]);

  octaveOffset = 36 + octaveShift;
  /* Loop over all the events in the MIDI data */
  unsigned int nextEventMicros = myMicros();
  unsigned int lastEventMicros = nextEventMicros;
  int diffMicros;
  int nextEventTicks = (delta[0] < delta[1]) ? delta[0] : delta[1];
  lastEventMicros = nextEventMicros;
  //nextEventMicros += (nextEventTicks * qtrMicros / ticksPerQtr);
  /* Calculate the next event time floating point so some of the
     rounding errors will cancel */
  nextEventMicros += (int) (((float) (nextEventTicks * qtrMicros) /
			     (float) ticksPerQtr) + 0.5 );
  diffMicros = nextEventMicros - lastEventMicros;
  while (!eoTrk[0] || !eoTrk[1]) {
    for (track = 0; track < 2; track++) {
      //fprintf(stderr, "Track: %d\n", track);
      while ((delta[track] <= 0) && !eoTrk[track]) {
	parseEvent(&trk_p[track]);
	delta[track] = readVarLen(&trk_p[track]);
      }
    }
    if (eoTrk[0] && eoTrk[1]) break;
    /* Calculate the time to the next midi event from the number of ticks
       and wait until that time */
    nextEventTicks = (delta[0] < delta[1]) ? delta[0] : delta[1];
    //fprintf(stderr, "Micros: %lu, Incr: %lu\n", nextEventMicros, nextEventTicks * qtrMicros / ticksPerQtr);
    lastEventMicros = nextEventMicros;
    nextEventMicros +=  (nextEventTicks * qtrMicros / ticksPerQtr);
    diffMicros = nextEventMicros - lastEventMicros;
    //fprintf(stderr, "Next: %lu, Micros: %lu, Delta[0] %d, Delta[1] %d\n", nextEventMicros, micros(), delta[0], delta[1]);
    while ((myMicros() - lastEventMicros) < diffMicros) {  }
    delta[0] -= nextEventTicks;
    delta[1] -= nextEventTicks;
    //fprintf(stderr, "D0: %d,  D1: %d\n", delta[0], delta[1]);
  }

  munmap(midi_p, sb.st_size);
  close(midi_d);
  prevSws[2] ^= 0xff;
  lcdMillis = myMillis();
}

int readVarLen(char **ptr) {
  /* Parses variable length values in MIDI data pointed to by the pointer
     argument and updates the pointer to the byte after the variable value */
  int data = 0;
  while (*ptr[0] & 0x80) {
    //fprintf(stderr, "%2.2X ", *ptr[0]);
    data |= *ptr[0] & 0x7f;
    ++*ptr;
    data <<= 7;
  }
  //fprintf(stderr, "%2.2X ", *ptr[0]);
  data |= *ptr[0];
  ++*ptr;
  //fprintf(stderr, "Data value: %d\n", data);
  return data;
}

void parseEvent(char **ptr) {
  /* parses and acts on the MIDI event pointed to by the argument
     and updates the pointer to the next event */
  //fprintf(stderr, "Delta: %d  Event %2.2X  ", delta[track], **ptr);
  if (0xff == **ptr) {
    /* META events */
    ++*ptr;
    //fprintf(stderr, "Meta event: FF %2.2X  ", **ptr);
    if (0x2f == **ptr) {
      /* End of track */
      eoTrk[track] = 1;
      ++*ptr;
      readVarLen(ptr); // to update the pointer to the next event
      //fprintf(stderr, "\n");

    } else if (0x51 == **ptr) {
      /* Set tempo (microseconds per crotchet)
	 Always 3 bytes so skip over length data */
      *ptr += 2;
      qtrMicros = 0;
      for (uint8_t i = 0; i < 3; i++) {
	qtrMicros <<= 8;
	qtrMicros |= **ptr;
	++*ptr;
      }
      //fprintf(stderr, "Tick length: %d\n", qtrMicros);
    } else {
      /* We don't need to worry about other meta events, so skip over them */
      ++*ptr;
      int len = readVarLen(ptr);
      *ptr += len;
      //fprintf(stderr, "\n");
    }
    
  /* Other MIDI events from here onwards*/
  } else if (0xE0 == (**ptr & 0xf0)) {
    /* Pitch Wheel Change (2 bytes - lsb, msb) */
    int pitch = (*(*ptr + 2) & 0x7f);
    pitch <<= 7;
    pitch |= (*(*ptr + 1) & 0x7f);
    //fprintf(stderr, "Pitch Change: 0x%4.4X\n", pitch);
    /* Ruban mode so send data as /midiRbn
       Absolute pitch with 8192 equivalent to middle C (midi 60)
       Allow for PD adding the octave offset to this value) */
    if (ruban) {
      lo_send(pd_lo, "/midiRbn", "f", (float) pitch / 170.6666667 - 24.0 - (float) octaveShift);
    } else {
      /* Clavier mode so send vibrato - 8192 is 0 offset
	 Need to calibrate this to give a sensible range;
	 it's divided by 25 in PD - and note that the accelerometer
	 sends -1 when static */
      lo_send(pd_lo, "/vib", "i", pitch - 8193);
    }
    *ptr += 3;

  } else if (0xC0 == (**ptr & 0xf0)) {
    /* Program Change - 7 bits of data spread
       over 2 of the 3 bytes to be sent to PD
       Zero and then set the relevant bits */
    //fprintf(stderr, "Program Change: 0x%2.2X\n", *(*ptr + 1) & 0x7f);
    midiSws[0] &= 0x03;
    midiSws[0] |= (*(*ptr + 1) & 0x1f) << 3;
    midiSws[1] &= 0xfc;
    midiSws[1] |= (*(*ptr + 1) & 0x60) >> 5;
    lo_send(pd_lo, "/sw", "iii", midiSws[0], midiSws[1], midiSws[2]);
    *ptr += 2;

  } else if (0xB0 == (**ptr & 0xf0)) {
    /* Control Change (2 bytes - controller, value)
       Controller is *(*ptr + 1), data values in *(*ptr + 2) */
    switch (*(*ptr + 1)) {
    case 0x0B: /* Expression Controller MSB*/
      //fprintf(stderr, "Expression: 0x%2.2X", *(*ptr + 2) & 0x7f);
      analogueVal[6] = (int) (((*(*ptr + 2) & 0x07f) * 992) / 383);
      //fprintf(stderr, "Expression: %d\n", analogueVal[6]);
      lo_send(pd_lo, "/anlg", "iiiiiiii",
	      analogueVal[0], analogueVal[1], analogueVal[2],
	      analogueVal[3], analogueVal[4], analogueVal[5],
	      analogueVal[6], analogueVal[7]);
      break;

    case 0x10: /* GP Controller 1 (octaviant level) */
    case 0x11: /* GP Controller 2 (petit gambe level) */
    case 0x12: /* GP Controller 3 (souffle level) */
    case 0x13: /* GP Controller 4 (effect diffuseur level) */
      /* For these 4 controllers, work out the analogue value
	 to be changed from the controller number */
      analogueVal[*(*ptr + 1) - 14] = (*(*ptr + 2) & 0x07f) << 3;
      lo_send(pd_lo, "/anlg", "iiiiiiii",
	      analogueVal[0], analogueVal[1], analogueVal[2],
	      analogueVal[3], analogueVal[4], analogueVal[5],
	      analogueVal[6], analogueVal[7]);     
      break;

    case 0x50: /* GPC 5 - Diffuseur selection */
      midiSws[1] &= 0x0f; // zero the existing Diffuseur selection
      midiSws[1] |= (*(*ptr + 2) & 0x7f) << 4;
      lo_send(pd_lo, "/sw", "iii", midiSws[0], midiSws[1], midiSws[2]);
      break;

    case 0x51: /* GPC 6 - clavier / ruban mode */
      /* Determines how pitchbend information is processed
	 In Clavier mode pitch bind is interpreted and sent as /vib data
	 but in Ruban mode as analogueVal[1] */
      ruban = ((*(*ptr + 2) & 0x7f) < 64) ? 0 : 1;
      midiSws[1] &= 0xfb; // zero the existing C/R selection
      if (ruban) midiSws[1] |= 4;
      lo_send(pd_lo, "/sw", "iii", midiSws[0], midiSws[1], midiSws[2]);
      break;

    case 0x52: /* GPC 7 - legato / claquement mode */
      claquement = ((*(*ptr + 2) & 0x7f) < 64) ? 0 : 1;
      midiSws[1] &= 0xf7; // zero the existing L/C selection
      if (claquement) midiSws[1] |= 8;
      lo_send(pd_lo, "/sw", "iii", midiSws[0], midiSws[1], midiSws[2]);
      break;

    case 0x53: /* GPC 8 - Feutre pedal analogue value */
      analogueVal[7] = (int) (*(*ptr + 2) & 0x07f) << 3;
      lo_send(pd_lo, "/anlg", "iiiiiiii",
	      analogueVal[0], analogueVal[1], analogueVal[2],
	      analogueVal[3], analogueVal[4], analogueVal[5],
	      analogueVal[6], analogueVal[7]);     
      break;
    }
    //fprintf(stderr, "\n");
    *ptr += 3;

  } else if (0x90 == (**ptr & 0xf0)) {
    /* Note On (2 bytes - note, velocity) */
    //fprintf(stderr, "Note On: %d\n", *(*ptr + 1) & 0x7f);
    lo_send(pd_lo, "/key", "ii", (*(*ptr + 1) & 0x7f) - octaveOffset, 1);
    *ptr += 3;

  } else if (0x80 == (**ptr & 0xf0)) {
    /* Note Off (2 bytes - note, velocity) */
    //fprintf(stderr, "Note Off: %d\n", *(*ptr + 1) & 0x7f);
    if (claquement)
      lo_send(pd_lo, "/key", "ii", (*(*ptr + 1) & 0x7f) - octaveOffset, 0);
    *ptr += 3;
  }
}

void selectMidiFile(void) {
  /* Free any previously allocated memory */
  if (NULL != midiFile) {
    for (uint8_t i = 0; i <= midiCount; i++) {
      free(midiFile[i]);
    }
    free(midiFile);
  }

  /* Read the MIDI directory to find the available files */
  DIR *dir = opendir("/home/pi/Ondes/MIDI");
  struct dirent *ent;
  midiCount = 0;
  /* find out how many .mid files there are */
  while (ent = readdir(dir)) {
    if ((ent->d_type == DT_REG) && strncmp(ent->d_name, ".mid", 4)) {
      ++midiCount;
    }
  }
  rewinddir(dir);
  /* Read the MIDI file names */
  midiFile = malloc(sizeof(char *) * (midiCount + 1));
  midiFile[0] = malloc(7 * sizeof(char));
  strcpy(midiFile[0], "Cancel");
  for (uint8_t i = 1; ent = readdir(dir);) {
    uint8_t index = i;
    if ((ent->d_type == DT_REG) && strncmp(ent->d_name, ".mid", 4)) {
      /* Make space for the new entry at the end of the list */
      midiFile[i] = malloc((strlen(ent->d_name) + 1) * sizeof(char));
      strcpy(midiFile[i], ent->d_name);
      char *tmpFile = midiFile[i];
      /* If necessary rearrange the pointers to existing entries
	 to fit the new one in alphabetically */
      for (uint8_t j = i - 1;
	   (strcasecmp(midiFile[j], ent->d_name) > 0) && (j > 0); j--) {
	midiFile[j + 1] = midiFile[j];
	index = j;
      }
      midiFile[index] = tmpFile;
      ++i;
    }
  }
  closedir(dir);
  /* Now we have the available MIDI files so select the required one */
  if (midiSel > midiCount) midiSel = 0;
  lcd1602SetCursor(0, 1);
  lcd1602WriteString("                ");
  lcd1602SetCursor(0, 1);
  lcd1602WriteString(midiFile[midiSel]);
  uint8_t clicked = 0;
  while (!encoderPress()) {
    if ((clicks = encoderRotate())) {
      midiSel += clicks / abs(clicks);
      if (midiSel > midiCount) {
	midiSel = 0;
      } else if (midiSel < 0) {
	midiSel = midiCount;
      }
      lcd1602SetCursor(0, 1);
      lcd1602WriteString("                ");
      lcd1602SetCursor(0, 1);
      char tmpName[17];
      strncpy(tmpName, midiFile[midiSel], 16);
      lcd1602WriteString(tmpName);
    }
  }
  lcd1602SetCursor(0, 1);
  if (midiSel) {
    playMidi = 1;
    lcd1602WriteString("Play MIDI Play  ");
  } else {
    playMidi = 0;
    lcd1602WriteString("Play MIDI No    ");
  }
}

void gpioSetMode(uint8_t gpio, uint8_t mode) {
  int reg, shift;

  reg   = gpio / 10;
  shift = (gpio % 10) * 3;

  gpioReg[reg] = (gpioReg[reg] & ~(7 << shift)) | (mode << shift);
}

uint8_t gpioGetMode(uint8_t gpio) {
  int reg, shift;

  reg   =  gpio / 10;
  shift = (gpio % 10) * 3;

  return (*(gpioReg + reg) >> shift) & 7;
}

void gpioSetPullUpDown(uint8_t gpio, uint8_t pud) {
  *(gpioReg + GPPUD) = pud;

  usleep(20);

  *(gpioReg + GPPUDCLK0 + PI_BANK) = PI_BIT;

  usleep(20);

  *(gpioReg + GPPUD) = 0;

  *(gpioReg + GPPUDCLK0 + PI_BANK) = 0;
}

uint8_t gpioRead(uint8_t gpio) {
   if ((*(gpioReg + GPLEV0 + PI_BANK) & PI_BIT) != 0)
     return 1;
   else
     return 0;
}

void gpioWrite(uint8_t gpio, uint8_t level) {
  if (level == 0)
    *(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
  else
    *(gpioReg + GPSET0 + PI_BANK) = PI_BIT;
}

void gpioTrigger(uint8_t gpio, uint16_t pulseLen, uint8_t level) {
  if (level == 0)
    *(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
  else
    *(gpioReg + GPSET0 + PI_BANK) = PI_BIT;

  usleep(pulseLen);

  if (level != 0)
    *(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
  else
    *(gpioReg + GPSET0 + PI_BANK) = PI_BIT;
}

/* Bit (1<<x) will be set if gpio x is high. */
uint32_t gpioReadBank1(void) {
  return (*(gpioReg + GPLEV0));
}
uint32_t gpioReadBank2(void) {
  return (*(gpioReg + GPLEV1));
}

/* To clear gpio x bit or in (1<<x). */
void gpioClearBank1(uint32_t bits) {
  *(gpioReg + GPCLR0) = bits;
}
void gpioClearBank2(uint32_t bits) {
  *(gpioReg + GPCLR1) = bits;
}

/* To set gpio x bit or in (1<<x). */
void gpioSetBank1(uint32_t bits) {
  *(gpioReg + GPSET0) = bits;
}
void gpioSetBank2(uint32_t bits) {
  *(gpioReg + GPSET1) = bits;
}

/*uint16_t gpioHardwareRevision(void) {
  static unsigned rev = 0;

  FILE * filp;
  char buf[512];
  char term;
  int chars=4; /* number of chars in revision string */

/* if (rev) return rev;

  piModel = 0;

  filp = fopen ("/proc/cpuinfo", "r");

  if (filp != NULL) {
    while (fgets(buf, sizeof(buf), filp) != NULL) {
      if (piModel == 0) {
	if (!strncasecmp("model name", buf, 10)) {
	  if (strstr (buf, "ARMv6") != NULL) {
	    piModel = 1;
	    chars = 4;
	  } else if (strstr (buf, "ARMv7") != NULL) {
	    piModel = 2;
	    chars = 6;
	  } else if (strstr (buf, "ARMv8") != NULL) {
	    piModel = 2;
	    chars = 6;
	  }
	}
      }

      if (!strncasecmp("revision", buf, 8)) {
	if (sscanf(buf+strlen(buf)-(chars+1), "%x%c", &rev, &term) == 2) {
	  if (term != '\n') rev = 0;
	}
      }
    }

    fclose(filp);
  }
  return rev;
}*/

int8_t gpioInitialise(void) {
  int fd;

  //piRev = gpioHardwareRevision(); /* sets piModel and piRev */

  fd = open("/dev/gpiomem", O_RDWR | O_SYNC) ;

  if (fd < 0) {
    fprintf(stderr, "failed to open /dev/gpiomem\n");
    return -1;
  }

  gpioReg = (uint32_t *)mmap(NULL, 0xB4, PROT_READ|PROT_WRITE,
			     MAP_SHARED, fd, 0);

  close(fd);

  if (gpioReg == MAP_FAILED) {
    fprintf(stderr, "Bad, mmap failed\n");
    return -1;
  }
  return 0;
}

uint8_t mcp23s08_read_reg(uint8_t reg, uint8_t hw_addr, int fd) {
  uint8_t control_byte = get_spi_control_byte(READ_CMD, hw_addr);
  uint8_t tx_buf[3] = {control_byte, reg, 0};
  uint8_t rx_buf[sizeof tx_buf];

  struct spi_ioc_transfer spi;
  memset (&spi, 0, sizeof(spi));
  spi.tx_buf = (unsigned long) tx_buf;
  spi.rx_buf = (unsigned long) rx_buf;
  spi.len = sizeof tx_buf;
  spi.delay_usecs = spi_delay;
  spi.speed_hz = spi_speed;
  spi.bits_per_word = spi_bpw;

  // do the SPI transaction
  if ((ioctl(fd, SPI_IOC_MESSAGE(1), &spi) < 0)) {
    fprintf(stderr,
            "mcp23s08_read_reg: There was an error during the SPI transaction.\n");
    return -1;
  }

  // return the data
  return rx_buf[2];
}

void mcp23s08_write_reg(uint8_t data, uint8_t reg, uint8_t hw_addr, int fd) {
  uint8_t control_byte = get_spi_control_byte(WRITE_CMD, hw_addr);
  uint8_t tx_buf[3] = {control_byte, reg, data};
  uint8_t rx_buf[sizeof tx_buf];

  struct spi_ioc_transfer spi;
  memset (&spi, 0, sizeof(spi));
  spi.tx_buf = (unsigned long) tx_buf;
  spi.rx_buf = (unsigned long) rx_buf;
  spi.len = sizeof tx_buf;
  spi.delay_usecs = spi_delay;
  spi.speed_hz = spi_speed;
  spi.bits_per_word = spi_bpw;

  // do the SPI transaction
  if ((ioctl(fd, SPI_IOC_MESSAGE(1), &spi) < 0)) {
    fprintf(stderr,
            "mcp23s08_write_reg: There was an error during the SPI transaction.\n");
  }
}

static uint8_t get_spi_control_byte(uint8_t rw_cmd, uint8_t hw_addr) {
  hw_addr = (hw_addr << 1) & 0xE;
  rw_cmd &= 1; // just 1 bit long
  return 0x40 | hw_addr | rw_cmd;
}
