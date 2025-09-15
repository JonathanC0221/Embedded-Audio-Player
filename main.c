/*=========================================================================*/
/*  Includes                                                               */
/*=========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <system.h>
#include <sys/alt_alarm.h>
#include <io.h>

#include "fatfs.h"
#include "diskio.h"

#include "ff.h"
#include "monitor.h"
#include "uart.h"

#include "alt_types.h"

#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>

#include "altera_avalon_pio_regs.h"
#include "altera_avalon_timer_regs.h"
#include "sys/alt_irq.h"

volatile int buttons; //checking current state
volatile int counter;
volatile int want_button_state; // 1 = release, 0 = press
volatile int timer = 0;
volatile int stable_button;
volatile int play = 0;
volatile int stop = 1;
volatile int incr = 0;
volatile int list_size;
volatile int seek = 0;
volatile int display = 1;
volatile int action = 0;

static void button_isr (void* context, alt_u32 id)
{
	//buttons = IORD(BUTTON_PIO_BASE, 0);
	want_button_state = 0; // interrupt called when button pressed (0)
	IOWR(BUTTON_PIO_BASE, 3, 0X0); // clear interrupt - edge capture
	IOWR(BUTTON_PIO_BASE, 2, 0x0); // disable further button interrupts until counter is done and button is stably released
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);// start timer - bit 2 of control reg = 1, bit 1 (cont) should be 0, bit 0 should be 1 = 5-> timer interrupt generated when done counting and ITO of control = 0
}

static void timer_isr (void* context, alt_u32 id)
{
	IOWR_ALTERA_AVALON_TIMER_STATUS(TIMER_0_BASE, 0); // clear interrupt - clear TO bit of status reg - bit 0
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8); // stop just in case (write to bit 3 to stop)


	buttons = IORD(BUTTON_PIO_BASE, 0); //grab current button value
	if ((buttons == 15 && want_button_state == 0) || (buttons != 15 && want_button_state == 1)){ //button is not pressed but we want it to be or vice versa
		counter = 0; // must wait for signal to be low for continuous amount of time so reset
		IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5); // rerun timer
	}
	else if (buttons != 15 && want_button_state == 0) { //matching - start counting number of times it's stable, if not stable, counter will be reset
		if (counter < 20){
			counter++;
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5); // rerun timer
		}
		else if (counter == 20){ // considered stabilized
			counter = 0; // reset for next press
			want_button_state = 1; // once stably pressed down  we are waiting for a release
			stable_button = buttons; // signals that button is stably low
			if (stable_button == 13) { //pause or play
				action = 1;
			} else if (stable_button == 11) { //stop
				action = 2;
			} else if (stable_button == 14) { //forward
				action = 3;
			} else if (stable_button == 7) { //back
				action = 4;
			} // determine action once press down debounced
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5); // continue calling timer to initiate release debouncing
		}
	}
	else if (buttons == 15 && want_button_state == 1) { //matching release - start counting
		if (counter < 20){ //start counting number of times it's stable when button is released
			counter++;
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5); // rerun timer
		}
		else if (counter == 20){
			//carry out action upon release
			if (action == 1) { //pause or play
							// play or pause based on prev state
							if (play) {
								play = 0;
							} else {
								play = 1;
								stop = 0;
							}
						} else if (action == 2) {
							stop = 1; //stop
						} else if (action == 3) { //incr is current song index, seek forward by +1
							incr++;
							seek = 1;
							if (incr >= list_size) { // loop around if reached end of playlist
								incr = 0;
							}
						} else if (action == 4) { //seek back by -1
							incr--;
							seek = 1;
							if (incr < 0) { //loop back to end of list
								incr = list_size-1;
							}
						}
			//reset everything and ready display once done
			action = 0;
			stable_button = 15;
			counter = 0;
			display = 1;
			IOWR(BUTTON_PIO_BASE, 2, 0b1111); //re-enable button interrupt to wait for next press
		}
	}
}

/*=========================================================================*/
/*  DEFINE: All Structures and Common Constants                            */
/*=========================================================================*/

/*=========================================================================*/
/*  DEFINE: Macros                                                         */
/*=========================================================================*/

#define PSTR(_a)  _a

#define ESC 27
#define CLEAR_LCD_STRING "[2J"

/*=========================================================================*/
/*  DEFINE: Prototypes                                                     */
/*=========================================================================*/

/*=========================================================================*/
/*  DEFINE: Definition of all local Data                                   */
/*=========================================================================*/
static alt_alarm alarm;
static unsigned long Systick = 0;
static volatile unsigned short Timer;   /* 1000Hz increment timer */

/*=========================================================================*/
/*  DEFINE: Definition of all local Procedures                             */
/*=========================================================================*/

/***************************************************************************/
/*  TimerFunction                                                          */
/*                                                                         */
/*  This timer function will provide a 10ms timer and                      */
/*  call ffs_DiskIOTimerproc.                                              */
/*                                                                         */
/*  In    : none                                                           */
/*  Out   : none                                                           */
/*  Return: none                                                           */
/***************************************************************************/
static alt_u32 TimerFunction (void *context)
{
   static unsigned short wTimer10ms = 0;

   (void)context;

   Systick++;
   wTimer10ms++;
   Timer++; /* Performance counter for this module */

   if (wTimer10ms == 10)
   {
      wTimer10ms = 0;
      ffs_DiskIOTimerproc();  /* Drive timer procedure of low level disk I/O module */
   }

   return(1);
} /* TimerFunction */

/***************************************************************************/
/*  IoInit                                                                 */
/*                                                                         */
/*  Init the hardware like GPIO, UART, and more...                         */
/*                                                                         */
/*  In    : none                                                           */
/*  Out   : none                                                           */
/*  Return: none                                                           */
/***************************************************************************/
static void IoInit(void)
{
   uart0_init(115200);

   /* Init diskio interface */
   ffs_DiskIOInit();

   //SetHighSpeed();

   /* Init timer system */
   alt_alarm_start(&alarm, 1, &TimerFunction, NULL);

} /* IoInit */

/*=========================================================================*/
/*  DEFINE: All code exported                                              */
/*=========================================================================*/

uint32_t acc_size;                 /* Work register for fs command */
uint16_t acc_files, acc_dirs;
FILINFO Finfo;
char Lfname[512];

char Line[256];                 /* Console input buffer */

FATFS Fatfs[_VOLUMES];          /* File system object for each logical drive */
FIL File1, File2;               /* File objects */
DIR Dir;                        /* Directory object */
uint8_t Buff[1024] __attribute__ ((aligned(4)));  /* Working buffer */




static
FRESULT scan_files(char *path)
{
    DIR dirs;
    FRESULT res;
    uint8_t i;
    char *fn;


    if ((res = f_opendir(&dirs, path)) == FR_OK) {
        i = (uint8_t)strlen(path);
        while (((res = f_readdir(&dirs, &Finfo)) == FR_OK) && Finfo.fname[0]) {
            if (_FS_RPATH && Finfo.fname[0] == '.')
                continue;
#if _USE_LFN
            fn = *Finfo.lfname ? Finfo.lfname : Finfo.fname;
#else
            fn = Finfo.fname;
#endif
            if (Finfo.fattrib & AM_DIR) {
                acc_dirs++;
                *(path + i) = '/';
                strcpy(path + i + 1, fn);
                res = scan_files(path);
                *(path + i) = '\0';
                if (res != FR_OK)
                    break;
            } else {
                //      xprintf("%s/%s\n", path, fn);
                acc_files++;
                acc_size += Finfo.fsize;
            }
        }
    }

    return res;
}


//                put_rc(f_mount((uint8_t) p1, &Fatfs[p1]));

static
void put_rc(FRESULT rc)
{
    const char *str =
        "OK\0" "DISK_ERR\0" "INT_ERR\0" "NOT_READY\0" "NO_FILE\0" "NO_PATH\0"
        "INVALID_NAME\0" "DENIED\0" "EXIST\0" "INVALID_OBJECT\0" "WRITE_PROTECTED\0"
        "INVALID_DRIVE\0" "NOT_ENABLED\0" "NO_FILE_SYSTEM\0" "MKFS_ABORTED\0" "TIMEOUT\0"
        "LOCKED\0" "NOT_ENOUGH_CORE\0" "TOO_MANY_OPEN_FILES\0";
    FRESULT i;

    for (i = 0; i != rc && *str; i++) {
        while (*str++);
    }
    xprintf("rc=%u FR_%s\n", (uint32_t) rc, str);
}

int isWav(char *filename) { //takes filename and compares last 4 characters. if it matches .WAV, we add to the list
	char temp [5]= {filename[strlen(filename)-4], filename[strlen(filename)-3], filename[strlen(filename) -2], filename[strlen(filename)-1], '\0'};
	if (strcmp(".WAV", temp) == 0) {
		return 1;
	} else {
		return 0;
	}
}

static void TestLCD(char fname[20], int sd_ind, int speed, int mono)
{
  FILE *lcd;
  lcd = fopen("/dev/lcd_display", "w");

  /* Write some simple text to the LCD. */
  if (lcd != NULL )
  {
    fprintf(lcd, "%d %s\n", sd_ind, fname);
    if (stop == 1) {
    	fprintf(lcd, "STOPPED\n");
    } else if (play == 0) {
    	fprintf(lcd, "PAUSED\n");
    } else {
    	if (mono == 1) {
    		fprintf(lcd, "PBACK-MONO\n");
    	} else if (speed == 8) {
    		fprintf(lcd, "PBACK-DBL SPD\n");
    	} else if (speed == 2){
    		fprintf(lcd, "PBACK-HLF SPD\n");
    	}
    	else if (speed == 4) {
    		fprintf(lcd, "PBACK-NORM SPD\n");
    		}
    	}
    }
  fclose(lcd);
}

/***************************************************************************/
/*  main                                                                   */
/***************************************************************************/
int main(void)
{
	alt_irq_register( BUTTON_PIO_IRQ, (void *)0, button_isr ); // register isr
	  alt_irq_register( TIMER_0_IRQ, (void *)0, timer_isr );

	  IOWR(BUTTON_PIO_BASE, 2, 0b1111); //enable button interrupt (write 1s to interrupt mask)
	  IOWR_ALTERA_AVALON_TIMER_STATUS(TIMER_0_BASE, 0);// set TO bit of status reg to 0 (clearing interrupt just in case)
	  IOWR_ALTERA_AVALON_TIMER_PERIODH(TIMER_0_BASE, 0x0); // timer period = number of clock cycles ?
	  IOWR_ALTERA_AVALON_TIMER_PERIODL(TIMER_0_BASE, 0xffff);
	  IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8); //stop timer just in case
	  IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5); // start timer and enable interrupts

	int fifospace;
    char *ptr, *ptr2;
    long p1, p2, p3;
    uint8_t res, b1, drv = 0;
    uint16_t w1;
    uint32_t s1, s2, cnt, blen = sizeof(Buff);
    static const uint8_t ft[] = { 0, 12, 16, 32 };
    uint32_t ofs = 0, sect = 0, blk[2];
    FATFS *fs;                  /* Pointer to file system object */
    char song_name[20][20];
    unsigned int song_size[20];
    int sd_ind[20];
    int sd_ind_ind = 1;
    list_size = 0;

    alt_up_audio_dev * audio_dev;
    /* used for audio record/playback */
    unsigned int l_buf;
    unsigned int r_buf;
    // open the Audio port
    audio_dev = alt_up_audio_open_dev ("/dev/Audio");
    if ( audio_dev == NULL)
    alt_printf ("Error: could not open audio device \n");
    else
    alt_printf ("Opened audio device \n");

    IoInit();

//    xputs(PSTR("FatFs module test monitor\n"));
//    xputs(_USE_LFN ? "LFN Enabled" : "LFN Disabled");
//    xprintf(", Code page: %u\n", _CODE_PAGE);

    xprintf("rc=%d\n", (uint16_t) disk_initialize((uint8_t) 0));
    put_rc(f_mount((uint8_t) 0, &Fatfs[0])); //mount disk

    res = f_opendir(&Dir, 0);
    if (res) // if res in non-zero there is an error; print the error.
    {
    	put_rc(res);
    	exit(1);
    }
    p1 = s1 = s2 = 0; // otherwise initialize the pointers and proceed.
    for (;;)
    {
    	res = f_readdir(&Dir, &Finfo);
    	if ((res != FR_OK) || !Finfo.fname[0])
    		break;
    	if (Finfo.fattrib & AM_DIR)
    	{
    		s2++;
    	}
    	else
    	{
    		s1++;
    		p1 += Finfo.fsize;
    	}
    	xprintf("%9lu  %s", Finfo.fsize, &(Finfo.fname[0]));

    	if (isWav(&(Finfo.fname[0]))) {
    		strcpy(song_name[list_size], &(Finfo.fname[0])); //song name
    		song_size[list_size] = Finfo.fsize; //size of song
    		sd_ind[list_size] = sd_ind_ind; //index of sd card
    		list_size++;
    	}

    	xputc('\n');
    	sd_ind_ind++; //increase index of sdcard with new loop iteration
    }
    xprintf("%4u File(s),%10lu bytes total\n%4u Dir(s)", s1, p1, s2);

    res = f_getfree(0, (uint32_t *) & p1, &fs);
    if (res == FR_OK)
    	xprintf(", %10lu bytes free\n", p1 * fs->csize * 512);
    else
    	put_rc(res);

    while (1) { //main while loop
    	if (display && stop) { //if ready ti display and in stopped state, display
    		TestLCD(song_name[incr], sd_ind[incr], 0, 0);
    		display = 0;
    	}
    	if (!stop) { //not in stopped state
    		put_rc(f_open(&File1, song_name[incr], (uint8_t) 1)); //open audio file at increment
    		ofs = File1.fptr;
    		int i = 0;
    		p1 = song_size[incr]; //selected song's size
    		int mono = 0;
    		int speed = 4;
    		int switches;
    		switches = IORD(SWITCH_PIO_BASE, 0); //switch values
    		switches = switches & 0b11; //grab only switch 0 and 1 values
    		if (switches == 1){
    			speed = 2;
    		}
    		else if (switches == 2) {
    			speed = 8;
    		}
    		else if (switches == 3) {
    			mono = 1;
    		}

    		TestLCD(song_name[incr], sd_ind[incr], speed, mono); //display current speed
    		display = 0; //set display flag to 0
    		while (p1)
    		{
    			if (stop || seek) {
    				break;
    			}
    			/*
				<<<<<<<<<<<<<<<<<<<<<<<<< YOUR fp CODE GOES IN HERE >>>>>>>>>>>>>>>>>>>>>>
    			 */
    			if ((uint32_t) p1 >= blen) { //bytes in sd greater than buffer length
    				cnt = blen; // read max number of bytes into buffer
    			}

    			else {
    				cnt = p1; //whatever is left
    				p1 = 0; // bytes left from sd card is 0
    			}

    			//**************
    			res = f_read(&File1, Buff, cnt, &s2);
    			if (cnt != s2){ // //error if cnt - requested read bytes - does not equal s2 (last index/actual num bytes read)referenced value
    				break;
    			}

    			uint32_t left_to_write = s2; //index in Buff of last data item
    			uint32_t to_write; // amount of data to write in each loop iteration
    			uint32_t right_avail;
    			uint32_t left_avail; // space in output fifo for each channel


    			while (left_to_write){ // while there is still audio to send
    				if (stop || seek){ //if stopped or seeking, break out of loop
    					break;
    				}
    				//monitoring fifo for space
    				right_avail = (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT))*speed; //converting available words in fifo space to "bytes". Bytes will vary based on desired speed
    				left_avail = (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT))*speed;
    				//need to align l and r - write same amount to both
    				if (right_avail < left_avail) {
    					to_write = right_avail;
    				}
    				else {
    					to_write = left_avail;
    				}

    				if (left_to_write < to_write){
    					to_write = left_to_write;
                               	}

//                               	if (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT) > 126) {
//                               		IOWR(LED_PIO_BASE,0, 0x1);
//                               		IOWR(LED_PIO_BASE,0, 0x0);
//                               	}

    				for (i = (s2 - left_to_write); i < (s2 - left_to_write + to_write); i+=speed){ //start at index of last read, increment by speed to determine which data is being read
    					if (display) { //if ready to display, show current status
    						TestLCD(song_name[incr], sd_ind[incr], speed, mono);
    						display = 0;
    					}
    					while (!play) { //paused, loop and do nothing unless stop is pressed. if pressed, break out to reset to beginning of loop
    						if (stop || seek) {break;}
    					} //stop when paused
    					if (stop || seek) {break;} //if playing a song and stop is pressed, break

    					r_buf = ((uint16_t)Buff[i+1] << 8) | (uint16_t)Buff[i]; //endian conversion
    					l_buf = ((uint16_t)Buff[i+3] << 8) | (uint16_t)Buff[i+2]; //endian conversion

    					if (mono == 1) { //if mono audio, write right channel data to both channels
    						alt_up_audio_write_fifo (audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
    						alt_up_audio_write_fifo (audio_dev, &(l_buf), 1, ALT_UP_AUDIO_RIGHT); // if playing one channel out of both

    					} else { //write audio
    						alt_up_audio_write_fifo (audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
    						alt_up_audio_write_fifo (audio_dev, &(r_buf), 1, ALT_UP_AUDIO_RIGHT);
    					}
    					p1 -= speed; //decrement sd card by "bytes" read
    				}
    				left_to_write -= to_write; //update data left to write in buffer
    			}
    		}
    		if (seek) {
    			if (play) {
    				stop = 0;
    				play = 1;
    			} else {
    				stop = 1;
    				play = 0;
    			}
    			seek = 0; //reset seek
    		} else {
    			stop = 1;
    			play = 0;
    		}
    		display = 1;
    	}
    }
    return (0);
	}
