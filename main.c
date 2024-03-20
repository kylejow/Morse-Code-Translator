#include <stdio.h>
#include <stdint.h>
#include "hw_types.h"
#include "hw_ints.h"
#include "hw_nvic.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "interrupt.h"
#include "hw_apps_rcm.h"
#include "prcm.h"
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"
#include "gpio.h"
#include "utils.h"
#include "systick.h"
#include "uart_if.h"
#include "pin_mux_config.h"
#include <stdbool.h>
#include "oled/Adafruit_GFX.h"
#include "oled/Adafruit_SSD1351.h"
#include "oled/glcdfont.h"
#include "oled/oled.h"
#include "spi.h"
#include "timer.h"
#include "timer_if.h"
#include "uart.h"
#include "ssl/ssl.h"
#include "common.h"

// conversions
#define ONE      0b00000110111110011010001001011101
#define TWO      0b00000110111110010000100011110111
#define THREE    0b00000110111110011000100001110111
#define FOUR     0b00000110111110011101000000101111
#define FIVE     0b00000110111110010001000011101111
#define SIX      0b00000110111110011001000001101111
#define SEVEN    0b00000110111110011110001000011101
#define EIGHT    0b00000110111110011101001000101101
#define NINE     0b00000110111110011111001000001101
#define ZERO     0b00000110111110010010000011011111
#define MUTE     0b00000110111110011110000000011111
#define LAST     0b00000110111110010110000010011111
#define VOL_UP   0b00000110111110010011000011001111
#define VOL_DOWN 0b00000110111110011011000001001111
#define CH_UP    0b00000110111110011000101001110101
#define CH_DOWN  0b00000110111110011001001001101101


const char* TWO_CHARS   = "ABC";
const char* THREE_CHARS = "DEF";
const char* FOUR_CHARS  = "GHI";
const char* FIVE_CHARS  = "JKL";
const char* SIX_CHARS   = "MNO";
const char* SEVEN_CHARS = "PQRS";
const char* EIGHT_CHARS = "TUV";
const char* NINE_CHARS  = "WXYZ";
const char* ZERO_CHARS  = " ";

const char *morseCode[] = {
    ".-",    // A
    "-...",  // B
    "-.-.",  // C
    "-..",   // D
    ".",     // E
    "..-.",  // F
    "--.",   // G
    "....",  // H
    "..",    // I
    ".---",  // J
    "-.-",   // K
    ".-..",  // L
    "--",    // M
    "-.",    // N
    "---",   // O
    ".--.",  // P
    "--.-",  // Q
    ".-.",   // R
    "...",   // S
    "-",     // T
    "..-",   // U
    "...-",  // V
    ".--",   // W
    "-..-",  // X
    "-.--",  // Y
    "--.."   // Z
};


extern void (* const g_pfnVectors[])(void);

volatile bool start = false;
volatile int currentBit = 0;
volatile uint64_t delta = 0;
volatile uint64_t delta_us = 0;
volatile int IR_Data = 0;
volatile bool read32Bits = false;
volatile int currentCycle = 0;
volatile int prevLetterButton = 0;
volatile int cursorPos = 0;
volatile char c;
volatile char message[20] = {0};
volatile int messageIndex = 0;
volatile char receivedMessage[20] = {0};
volatile uint32_t receivedIndex = 0;
volatile morsePending = false;
volatile heldCnt = 0;

typedef struct displayLine{
    volatile char text[256];
    volatile int lineNumber; // 0 to 11
} displayLine;


void printMorse(displayLine* line) {
    char c;
    int i;
    for (i = 0; i < 8; i++) {
        drawChar(((i%18)*7), 10*line->lineNumber+(i/18*10), ' ', BLUE, BLACK, 1);
    }
    for (i = 0; i < 8; i++) {
        c = line->text[i];
        drawChar(((i%18)*7), 10*line->lineNumber+(i/18*10), c, BLUE, BLACK, 1);
        if (c == ' ') {
            continue;
        }
        GPIO_IF_LedOn(MCU_GREEN_LED_GPIO);
        if (c == '.') {
            MAP_UtilsDelay(4000000);
        } else if (c == '-') {
            MAP_UtilsDelay(8000000);
        }
        GPIO_IF_LedOff(MCU_GREEN_LED_GPIO);
    }
}

void printLine(displayLine* line, unsigned int color) {
    char c;
    int i;
    for (i = 0; i < strlen(line->text); i++) {
        c = line->text[i];
        drawChar(((i%18)*7), 10*line->lineNumber+(i/18*10), c, color, BLACK, 1);
    }
}

void drawLastChar(displayLine* line) {
    int i = strlen(line->text);
    i = i == 0 ? 0 : i - 1;
    char c = line->text[i];
    drawChar(((i%18)*7), 10*line->lineNumber+(i/18*10), c, BLUE, BLACK, 1);
}

void deleteChar(displayLine* line) {
    int i = strlen(line->text);
    if (i == 0) {
        return;
    }
    i = i == 0 ? 0 : i - 1;
    char c = line->text[i];
    line->text[i] = '\0';
    drawChar(((i%18)*7), 10*line->lineNumber+(i/18*10), c, BLACK, BLACK, 1);
}

typedef struct PinSetting {
    unsigned long port;
    unsigned int pin;
} PinSetting;

// set pin62IRSensor as GPIO interrupt for IR sensor
static PinSetting pin62IRSensor = { .port = GPIOA0_BASE, .pin = 0x80};
static PinSetting pin4SW3 = { .port = GPIOA1_BASE, .pin = 0x20};
static PinSetting pin15SW2 = { .port = GPIOA2_BASE, .pin = 0x40};


// systick
#define SPI_IF_BIT_RATE  100000

// some helpful macros for systick

// the cc3200's fixed clock frequency of 80 MHz
// note the use of ULL to indicate an unsigned long long constant
#define SYSCLKFREQ 80000000ULL

// macro to convert ticks to microseconds
#define TICKS_TO_US(ticks) \
    ((((ticks) / SYSCLKFREQ) * 1000000ULL) + \
    ((((ticks) % SYSCLKFREQ) * 1000000ULL) / SYSCLKFREQ))\

// macro to convert microseconds to ticks
#define US_TO_TICKS(us) ((SYSCLKFREQ / 1000000ULL) * (us))

// systick reload value set to 40ms period
// (PERIOD_SEC) * (SYSCLKFREQ) = PERIOD_TICKS
#define SYSTICK_RELOAD_VAL 6400000ULL

//500ms
#define TIMER_RELOAD_VAL 80000000ULL


// track systick counter periods elapsed
// track timer counter periods elapsed
volatile int systick_cnt = 0;
volatile int timer0_cnt = 0;
volatile int timer1_cnt = 0;


/**
 * Reset SysTick Counter
 */
static inline void SysTickReset(void) {
    // any write to the ST_CURRENT register clears it
    // after clearing it automatically gets reset without
    // triggering exception logic
    // see reference manual section 3.2.1
    HWREG(NVIC_ST_CURRENT) = 1;

    // clear the global count variable
    systick_cnt = 0;
}

/**
 * SysTick Interrupt Handler
 *
 * Keep track of whether the systick counter wrapped
 */
static void SysTickHandler(void) {
    // increment every time the systick handler fires
    systick_cnt++;
}

/**
 * Initializes SysTick Module
 */
static void SysTickInit(void) {

    // configure the reset value for the systick countdown register
    MAP_SysTickPeriodSet(SYSTICK_RELOAD_VAL);

    // register interrupts on the systick module
    MAP_SysTickIntRegister(SysTickHandler);

    // enable interrupts on systick
    // (trigger SysTickHandler when countdown reaches 0)
    MAP_SysTickIntEnable();

    // enable the systick module itself
    MAP_SysTickEnable();
}

// timer
void TimerA0Handler(void)
{
    //
    // Clear the timer interrupt.
    //
    Timer_IF_InterruptClear(TIMERA0_BASE);
    timer0_cnt++;
}

void TimerA1Handler(void)
{
    //
    // Clear the timer interrupt.
    //
    Timer_IF_InterruptClear(TIMERA1_BASE);
    timer1_cnt++;
}

void timerInit(void) {
    //
    // Configuring the timers
    //
    Timer_IF_Init(PRCM_TIMERA0, TIMERA0_BASE, TIMER_CFG_PERIODIC, TIMER_A, 0);
    Timer_IF_Init(PRCM_TIMERA1, TIMERA1_BASE, TIMER_CFG_PERIODIC, TIMER_A, 0);

    //
    // Setup the interrupts for the timer timeouts.
    //
    Timer_IF_IntSetup(TIMERA0_BASE, TIMER_A, TimerA0Handler);
    Timer_IF_IntSetup(TIMERA1_BASE, TIMER_A, TimerA1Handler);

    //
    // Load number of ticks to timer
    //
    TimerLoadSet(TIMERA0_BASE, TIMER_A, TIMER_RELOAD_VAL);
    TimerLoadSet(TIMERA1_BASE, TIMER_A, TIMER_RELOAD_VAL);

    //
    // Enable interrupts
    //
    TimerIntEnable(TIMERA0_BASE, TIMER_TIMA_TIMEOUT);
    TimerIntEnable(TIMERA1_BASE, TIMER_TIMA_TIMEOUT);

    //
    // Enable timer
    //
    TimerEnable(TIMERA0_BASE, TIMER_A);
    TimerEnable(TIMERA1_BASE, TIMER_A);
}

static void IR_Handler(void) {
	unsigned long ulStatus;
    ulStatus = MAP_GPIOIntStatus (pin62IRSensor.port, true);
    MAP_GPIOIntClear(pin62IRSensor.port, ulStatus);

    if (systick_cnt > 0) {
        SysTickReset();
        start = false;
        currentBit = 0;
    }

    delta = SYSTICK_RELOAD_VAL - SysTickValueGet();
    delta_us = TICKS_TO_US(delta);

    if (delta_us > 11150 && delta_us < 11350) {
        heldCnt++;
    }

    if (delta_us > 13400 && delta_us < 13600) {
        start = true;
    }

    if (start) {
        if (delta_us > 1025 && delta_us < 1225) {
            IR_Data = (IR_Data << 1) | 0;
        }
        if (delta_us > 2150 && delta_us < 2350) {
            IR_Data = (IR_Data << 1) | 1;
        }
        currentBit++;
    }

    if (currentBit > 32) {
        currentBit = 0;
        read32Bits = true;
    }

    SysTickReset();
}
volatile int swFlag = 0;
volatile int sw3Flag = 0;
volatile int sw2Flag = 0;

void sw3Handler(void) {
    unsigned long ulStatus;
    ulStatus = MAP_GPIOIntStatus (pin4SW3.port, true);
    MAP_GPIOIntClear(pin4SW3.port, ulStatus);
    swFlag = 1;
    sw3Flag = 1;
}

void sw2Handler(void) {
    unsigned long ulStatus;
    ulStatus = MAP_GPIOIntStatus (pin15SW2.port, true);
    MAP_GPIOIntClear(pin15SW2.port, ulStatus);
    swFlag = 1;
    sw2Flag = 1;
}


displayLine rec = {.lineNumber = 9, .text = {'\0'}};

volatile size_t received_length = 0;
void UARTMultiRecieve(void) {
    uint32_t status = MAP_UARTIntStatus(UARTA1_BASE, true);
    MAP_UARTIntClear(UARTA1_BASE, status);

    while (MAP_UARTCharsAvail(UARTA1_BASE)) {
        char c = MAP_UARTCharGetNonBlocking(UARTA1_BASE);
        rec.text[received_length] = c;
        received_length++;
        if (c == '\n') {
            //printf("Received: %s\n", rec.text);
            printLine(&rec.text, BLUE);
            received_length = 0;
        }
    }
}

displayLine UARTMorse = {.lineNumber = 11, .text = {'\0'}};
void UARTMorseRecieve(void) {
    uint32_t status = MAP_UARTIntStatus(UARTA1_BASE, true);
    MAP_UARTIntClear(UARTA1_BASE, status);

    while (MAP_UARTCharsAvail(UARTA1_BASE)) {
        char c = MAP_UARTCharGetNonBlocking(UARTA1_BASE);
        UARTMorse.text[received_length] = c;
        received_length++;
        if (c == '\n') {
            UARTMorse.text[received_length - 1] = '\0';
            //printf("Received: %s\n", UARTMorse.text);
            printMorse(&UARTMorse.text);
            received_length = 0;
            memset(UARTMorse.text, '\0', sizeof(UARTMorse.text));
        }
    }
}


static void BoardInit(void) {
	MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
    
    // Enable Processor
    //
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);

    PRCMCC3200MCUInit();
}

bool isLetterButton(int button) {
    return button == TWO   ||
           button == THREE ||
           button == FOUR  ||
           button == FIVE  ||
           button == SIX   ||
           button == SEVEN ||
           button == EIGHT ||
           button == NINE  ||
           button == ZERO;
}

char* charToMorse(char c) {
    return morseCode[c - 'A'];
}

char *stringToMorse(const char *str) {
    int len = strlen(str);
    char *morseStr = (char *)malloc((4 * len + 1) * sizeof(char)); // Allocate memory for the Morse string
    if (morseStr == NULL) {
        //printf("Memory allocation failed!\n");
        return NULL;
    }

    morseStr[0] = '\0'; // Ensure the string is empty initially
    memset(morseStr, '\0', sizeof(morseStr));

    int i;
    for (i = 0; i < len; i++) {
        char c = str[i];
        if (c == ' ') {
            continue; // Skip spaces
        } else if (c >= 'A' && c <= 'Z') {
            strcat(morseStr, charToMorse(c));
            strcat(morseStr, "/"); // Add space between Morse characters
        }
    }

    return morseStr;
}

void padAndTerminateUARTMorse(char *input) {
    int i;
    for (i = strlen(input); i < 7; i++) {
        input[i] = ' ';
    }
    input[7] = '\n';
}



char morseToChar(const char *morse) {
    int i;
    for (i = 0; i < 26; i++) {
        if (strcmp(morse, morseCode[i]) == 0) {
            return 'A' + i;
        }
    }
    return '?';
}

void tokenizeString(char* input, char* delimiter, char* tokens[], int *numTokens){
    char tmp[256];
    strcpy(tmp, input);
    char* token = strtok(tmp, delimiter);
    int count = 0;
    while(token != NULL){
        tokens[count] = token;
        token = strtok(NULL, delimiter);
        count++;
    }
    *numTokens = count;
}

int main() {


    // initialize
	unsigned long ulStatus;

    BoardInit();
    
    PinMuxConfig();
    
    InitTerm();

    ClearTerm();    

    // Enable SysTick
    SysTickInit();

    //
    // Register the interrupt handlers
    //
    MAP_GPIOIntRegister(pin62IRSensor.port, IR_Handler);
    MAP_GPIOIntRegister(pin4SW3.port, sw3Handler);
    MAP_GPIOIntRegister(pin15SW2.port, sw2Handler);

    //
    // Configure falling edge interrupts
    //
    MAP_GPIOIntTypeSet(pin62IRSensor.port, pin62IRSensor.pin, GPIO_FALLING_EDGE);
    MAP_GPIOIntTypeSet(pin4SW3.port, pin4SW3.pin, GPIO_FALLING_EDGE);
    MAP_GPIOIntTypeSet(pin15SW2.port, pin15SW2.pin, GPIO_FALLING_EDGE);


    // clear interrupts on GPIOA2
    ulStatus = MAP_GPIOIntStatus (pin62IRSensor.port, true);
    MAP_GPIOIntClear(pin62IRSensor.port, ulStatus);
    ulStatus = MAP_GPIOIntStatus (pin4SW3.port, true);
    MAP_GPIOIntClear(pin4SW3.port, ulStatus);
    ulStatus = MAP_GPIOIntStatus (pin15SW2.port, true);
    MAP_GPIOIntClear(pin15SW2.port, ulStatus);

    // Enable interrupts
    MAP_GPIOIntEnable(pin62IRSensor.port, pin62IRSensor.pin);
    MAP_GPIOIntEnable(pin4SW3.port, pin4SW3.pin);
    MAP_GPIOIntEnable(pin15SW2.port, pin15SW2.pin);

    
    //
    // Enable the SPI module clock
    //
    MAP_PRCMPeripheralClkEnable(PRCM_GSPI,PRCM_RUN_MODE_CLK);

    
    //
    // Reset the peripheral
    //
    MAP_PRCMPeripheralReset(PRCM_GSPI);

    //
    // Reset SPI
    //
    MAP_SPIReset(GSPI_BASE);

    //
    // Configure SPI interface
    //
    MAP_SPIConfigSetExpClk(GSPI_BASE,MAP_PRCMPeripheralClockGet(PRCM_GSPI),
                     SPI_IF_BIT_RATE,SPI_MODE_MASTER,SPI_SUB_MODE_0,
                     (SPI_SW_CTRL_CS |
                     SPI_4PIN_MODE |
                     SPI_TURBO_OFF |
                     SPI_CS_ACTIVEHIGH |
                     SPI_WL_8));

    //
    // Enable SPI for communication
    //
    MAP_SPIEnable(GSPI_BASE);
    // init oled
    Adafruit_Init();
    // init timer
    timerInit();


    GPIO_IF_LedConfigure(LED1|LED2|LED3);

    GPIO_IF_LedOff(MCU_ALL_LED_IND);

    //Message("Initializing...\n\r");
    fillScreen(BLACK);
    
    long lRetVal = -1;

    //Connect the CC3200 to the local access point
    lRetVal = connectToAccessPoint();
    //Set time so that encryption can be used
    lRetVal = set_time();
    if(lRetVal < 0) {
        UART_PRINT("Unable to set time in the device");
        LOOP_FOREVER();
    }
    //Connect to the website with TLS encryption
    lRetVal = tls_connect();
    if(lRetVal < 0) {
        ERR_PRINT(lRetVal);
    }

    //Message("Ready\n\r");

    volatile bool enableUart = false;
    displayLine uartPrompt = {.lineNumber = 1, .text = "Enable UART?      SW3 = Y, SW2 = N"};
    printLine(&uartPrompt, BLUE);
    while (1) {
        if (swFlag) {
            if (sw3Flag) {
                enableUart = true;
                MAP_UARTConfigSetExpClk(UARTA1_BASE,SYSCLKFREQ, 
                UART_BAUD_RATE, (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                UART_CONFIG_PAR_NONE));

                MAP_UARTIntRegister(UARTA1_BASE, UARTMorseRecieve);

                ulStatus = MAP_UARTIntStatus(UARTA1_BASE, false);
                MAP_UARTIntClear(UARTA1_BASE, ulStatus);

                MAP_UARTIntEnable(UARTA1_BASE, UART_INT_RX);
                break;
            }
            if (sw2Flag) {
                break;
            }
        }
    }
    printLine(&uartPrompt, BLACK);
    
    volatile int mode = 0;

    volatile displayLine modeLine;
    strcpy(modeLine.text, "Decode Morse");
    modeLine.lineNumber = 0;
    printLine(&modeLine, BLUE);

    displayLine input = {.lineNumber = 2, .text = {'\0'}};
    displayLine output = {.lineNumber = 7, .text = {'\0'}};
    displayLine inputTitle = {.lineNumber = 1, .text = "Input:"};
    displayLine outputTitle = {.lineNumber = 6, .text = "Output:"};
    displayLine recievedTitle = {.lineNumber = 10, .text = "Recieved:"};
    printLine(&inputTitle, BLUE);
    printLine(&outputTitle, BLUE);
    if (enableUart)
        printLine(&recievedTitle, BLUE);

    sw3Flag = 0;
    sw2Flag = 0;
    swFlag = 0;
    
    while (1) {
        while (mode == 0) {
            if (swFlag) {
                if (sw3Flag) {
                    c = '.';
                    sw3Flag = 0;
                }
                if (sw2Flag) {
                    c = '-';
                    sw2Flag = 0;
                }
                input.text[strlen(input.text)] = c;
                drawLastChar(&input);
                swFlag = 0;
            }
            if (read32Bits) {
                if (IR_Data == ZERO) {
                    input.text[strlen(input.text)] = '/';
                    drawLastChar(&input);
                }
                if (IR_Data == TWO) {
                    timer1_cnt = 0;
                    heldCnt = 0;
                    TimerLoadSet(TIMERA1_BASE, TIMER_A, TIMER_RELOAD_VAL);
                    morsePending = true;
                }
                if (IR_Data == LAST) {
                    deleteChar(&input);
                }
                if (IR_Data == MUTE) {
                    printLine(&input, BLACK);
                    printLine(&output, BLACK);
                    memset(output.text, '\0', sizeof(input.text));
                    char* morseTokenized[256];
                    int numTokens = 0;
                    tokenizeString(input.text, "/", morseTokenized, &numTokens);
                    int i;
                    for (i = 0; i < numTokens; i++) {
                        c = morseToChar(morseTokenized[i]);
                        output.text[strlen(output.text)] = c;
                    }
                    printLine(&output, BLUE);
                    memset(input.text, '\0', sizeof(input.text));
                }
                if (IR_Data == ONE) {
                    mode = 1;
                    fillScreen(BLACK);
                    strcpy(modeLine.text, "Encode Morse");
                    printLine(&modeLine, BLUE);
                    printLine(&inputTitle, BLUE);
                    printLine(&outputTitle, BLUE);
                    if (enableUart)
                        printLine(&recievedTitle, BLUE);
                    memset(input.text, '\0', sizeof(input.text));

                }
                read32Bits = false;
                IR_Data = 0;
            }
            if (timer1_cnt > 0 && morsePending) {
                if (heldCnt > 4) {
                    c = '-';
                } else {
                    c = '.';
                }
                input.text[strlen(input.text)] = c;
                drawLastChar(&input);
                morsePending = false;
            }
        }
        while (mode == 1) {       
            // if read remote input
            if (read32Bits) {
                if (isLetterButton(IR_Data) && messageIndex < 18) {
                    // first new char
                    if (prevLetterButton == 0) {
                        timer0_cnt = 0;
                        TimerLoadSet(TIMERA0_BASE, TIMER_A, TIMER_RELOAD_VAL);
                        prevLetterButton = IR_Data;
                    // same char
                    } else if (IR_Data == prevLetterButton) {
                        TimerLoadSet(TIMERA0_BASE, TIMER_A, TIMER_RELOAD_VAL);
                        deleteChar(&input);
                        currentCycle++;
                    // new char
                    } else {
                        currentCycle = 0;
                        timer0_cnt = 0;
                        prevLetterButton = IR_Data;
                    }

                    if (IR_Data == TWO) {
                        c = TWO_CHARS[currentCycle%3];
                    } else if (IR_Data == THREE) {
                        c = THREE_CHARS[currentCycle%3];
                    } else if (IR_Data == FOUR) {
                        c = FOUR_CHARS[currentCycle%3];
                    } else if (IR_Data == FIVE) {
                        c = FIVE_CHARS[currentCycle%3];
                    } else if (IR_Data == SIX) {
                        c = SIX_CHARS[currentCycle%3];
                    } else if (IR_Data == SEVEN) {
                        c = SEVEN_CHARS[currentCycle%4];
                    } else if (IR_Data == EIGHT) {
                        c = EIGHT_CHARS[currentCycle%3];
                    } else if (IR_Data == NINE) {
                        c = NINE_CHARS[currentCycle%4];
                    } else if (IR_Data == ZERO) {
                        c = ' ';
                    } else {
                        c = NULL;
                    }

                    input.text[strlen(input.text)] = c;
                    drawLastChar(&input);

                } else if (IR_Data == LAST) {
                    deleteChar(&input);
                } else if (IR_Data == MUTE) {
                    //printf("sending message: %s\n", input.text);
                    int i;
                    char* morseConverted = stringToMorse(input.text);
                    printLine(&output, BLACK);
                    memset(output.text, '\0', sizeof(input.text));
                    strcpy(output.text, morseConverted);
                    printLine(&output.text, BLUE);
                    printLine(&input, BLACK);

                    if (enableUart) {
                        for (i = 0; i < strlen(input.text); i++) {
                            c = input.text[i];
                            if (c == ' ') {
                                continue;
                            }
                            int j;
                            char m[8] = {0};
                            strcpy(m, charToMorse(c));
                            padAndTerminateUARTMorse(&m);
                            for (j = 0; j < 8; j++) {
                                char mi = m[j];
                                MAP_UARTCharPut(UARTA1_BASE, mi);
                            }
                        }
                    }
                    http_post(lRetVal, morseConverted);
                    free(morseConverted);
                    memset(input.text, '\0', sizeof(input.text));
                } else if (IR_Data == ONE) {
                    mode = 0;
                    fillScreen(BLACK);
                    strcpy(modeLine.text, "Decode Morse");
                    printLine(&modeLine, BLUE);
                    printLine(&inputTitle, BLUE);
                    printLine(&outputTitle, BLUE);
                    if (enableUart)
                        printLine(&recievedTitle, BLUE);
                    memset(input.text, '\0', sizeof(input.text));
                    sw3Flag = 0;
                    sw2Flag = 0;
                    swFlag = 0;

                }
                read32Bits = false;
                IR_Data = 0;
            }

            // if letter is pending
            if (timer0_cnt > 0 && prevLetterButton != 0) {
                prevLetterButton = 0;
                timer0_cnt = 0;
                currentCycle = 0;
            }
        }
    }
}
