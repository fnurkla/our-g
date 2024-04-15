/*
 * See documentation at https://nRF24.github.io/RF24
 * See License information at root directory of this library
 * Author: Brendan Doherty (2bndy5)
 */

//#include <ctime>       // time()
//#include <iostream>    // cin, cout, endl
//#include <string>      // string, getline()
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>      // CLOCK_MONOTONIC_RAW, timespec, clock_gettime()
#include <rf24c.h> // RF24, RF24_PA_LOW, delay()

#define RF24_PA_LOW 1
/****************** Linux ***********************/
// Radio CE Pin, CSN Pin, SPI Speed
// CE Pin uses GPIO number with BCM and SPIDEV drivers, other platforms use their own pin numbering
// CS Pin addresses the SPI bus number at /dev/spidev<a>.<b>
// ie: RF24 radio(<ce_pin>, <a>*10+<b>); spidev1.0 is 10, spidev1.1 is 11 etc..
#define CSN_PIN 0
#ifdef MRAA
    #define CE_PIN 15 // GPIO22
#elif defined(RF24_WIRINGPI)
    #define CE_PIN 3 // GPIO22
#else
    #define CE_PIN 22
#endif
// Generic:
RF24Handle radio;
/****************** Linux (BBB,x86,etc) ***********************/
// See http://nRF24.github.io/RF24/pages.html for more information on usage
// See https://github.com/eclipse/mraa/ for more information on MRAA
// See https://www.kernel.org/doc/Documentation/spi/spidev for more information on SPIDEV

// For this example, we'll be using a payload containing
// a single float number that will be incremented
// on every successful transmission
float payload = 0.0;

void set_role(); // prototype to set the node's role
void master();  // prototype of the TX node's behavior
void slave();   // prototype of the RX node's behavior

// custom defined timer for evaluating transmission time in microseconds
struct timespec start_timer, end_timer;
uint32_t get_micros(); // prototype to get elapsed time in microseconds

int main(int argc, char** argv)
{
    radio = new_rf24(CE_PIN, CSN_PIN);

    // perform hardware check
    /* if (!rf24_begin(radio)) { */
    /*   printf("radio hardware is not responding!!\n"); */
    /*   return 0; // quit now */
    /* } */
    rf24_begin(radio);

    // to use different addresses on a pair of radios, we need a variable to
    // uniquely identify which address this radio will use to transmit
    int radio_number = 1; // 0 uses address[0] to transmit, 1 uses address[1] to transmit

    // print example's name
    printf("%s\n", argv[0]);

    // Let these addresses be used for the pair
    char address[2][6] = {"1Node", "2Node"};
    // It is very helpful to think of an address as a path instead of as
    // an identifying device destination

    // Set the radioNumber via the terminal on startup
    printf("Which radio is this? Enter '0' or '1'. ");
    int input;
    scanf("%d", &radio_number);

    // save on transmission time by setting the radio to only transmit the
    // number of bytes we need to transmit a float
    rf24_setPayloadSize(radio, sizeof(payload)); // float datatype occupies 4 bytes

    // Set the PA Level low to try preventing power supply related problems
    // because these examples are likely run with nodes in close proximity to
    // each other.
    rf24_setPALevel(radio, RF24_PA_LOW); // RF24_PA_MAX is default.

    // set the TX address of the RX node into the TX pipe
    rf24_openWritingPipe(radio, address[radio_number]); // always uses pipe 0

    // set the RX address of the TX node into a RX pipe
    rf24_openReadingPipe(radio, 1, address[!radio_number]); // using pipe 1

    // For debugging info
    // rf24_printDetails(radio);       // (smaller) function that prints raw register values
    // rf24_printPrettyDetails(radio); // (larger) function that prints human readable data

    // ready to execute program now
    set_role(); // calls master() or slave() based on user input
    return 0;
}

void set_role()
{
    char input = 0;
    while (input == 0) {
      printf("*** PRESS 'T' to begin transmitting to the other node\n");
      printf("*** PRESS 'R' to begin receiving from the other node\n");
      printf("*** PRESS 'Q' to exit\n");
      scanf("%c", input);
      if (input >= 1) {
	if (input == 'T' || input == 't')
	  master();
	else if (input == 'R' || input == 'r')
	  slave();
	else if (input == 'Q' || input == 'q')
	  break;
	else
	  printf("%c is an invalid input. Please try again.\n", input);
      }
      input = 0; // stay in the while loop
    }               // while
} // setRole()

void master()
{
    rf24_stopListening(radio); // put radio in TX mode

    unsigned int failure = 0; // keep track of failures
    while (failure < 6) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &start_timer);    // start the timer
	int report = rf24_write(radio, &payload, sizeof(float)); // transmit & save the report
        uint32_t timer_elapsed = get_micros();                // end the timer

        if (report) {
	  // payload was delivered
	  printf("Transmission successful! Time to transmit = %ld us. Sent: %f\n", timer_elapsed, payload);
	  payload += 0.01;                          // increment float payload
        }
        else {
            // payload was not delivered
	  printf("Transmission failed or timed out\n");
	  failure++;
        }

        // to make this example readable in the terminal
        sleep(1000); // slow transmissions down by 1 second
    }
    printf("%d failures detected. Leaving TX role.\n", failure);
}

void slave()
{

    rf24_startListening(radio); // put radio in RX mode

    time_t start_timer = time(NULL);       // start a timer
    while (time(NULL) - start_timer < 6) { // use 6 second timeout
        uint8_t pipe;
        if (rf24_available_pipe(radio, &pipe)) {                        // is there a payload? get the pipe number that recieved it
            uint8_t bytes = rf24_getPayloadSize(radio);          // get the size of the payload
            rf24_read(radio, &payload, bytes);                     // fetch payload from FIFO
            printf("Received %d bytes on pipe %d: %f\n", bytes, pipe, payload);      // print the size of the payload
            start_timer = time(NULL);                      // reset timer
        }
    }
    printf("Nothing received in 6 seconds. Leaving RX role.\n");
    rf24_stopListening(radio);
}

uint32_t get_micros()
{
    // this function assumes that the timer was started using
    // `clock_gettime(CLOCK_MONOTONIC_RAW, &startTimer);`

    clock_gettime(CLOCK_MONOTONIC_RAW, &end_timer);
    uint32_t seconds = end_timer.tv_sec - start_timer.tv_sec;
    uint32_t useconds = (end_timer.tv_nsec - start_timer.tv_nsec) / 1000;

    return ((seconds)*1000 + useconds) + 0.5;
}
