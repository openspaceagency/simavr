/*
	simduino.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>

#if __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif
#include <pthread.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "sim_elf.h"
#include "sim_hex.h"
#include "sim_gdb.h"
#include "sim_vcd_file.h"

#include "button.h"
#include "thermistor.h"
#include "uart_pty.h"

#define __AVR_ATmega644__
#include "marlin/pins.h"

#define PROGMEM
#define THERMISTORHEATER_0 5
#include "marlin/thermistortables.h"

thermistor_t	therm_hotend;
thermistor_t	therm_hotbed;
thermistor_t	therm_spare;
button_t button;
uart_pty_t uart_pty;
int do_button_press = 0;
avr_t * avr = NULL;
avr_vcd_t vcd_file;
uint8_t	pin_state = 0;	// current port B

float pixsize = 64;
int window;

/*
 * called when the AVR change any of the pins on port B
 * so lets update our buffer
 */
void pin_changed_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
//	pin_state = (pin_state & ~(1 << irq->irq)) | (value << irq->irq);
}

void displayCB(void)		/* function called whenever redisplay needed */
{
	// OpenGL rendering goes here...
	glClear(GL_COLOR_BUFFER_BIT);

	// Set up modelview matrix
	glMatrixMode(GL_MODELVIEW); // Select modelview matrix
	glLoadIdentity(); // Start with an identity matrix

	//float grid = pixsize;
	//float size = grid * 0.8;
    glBegin(GL_QUADS);
	glColor3f(1,0,0);

#if 0
	for (int di = 0; di < 8; di++) {
		char on = (pin_state & (1 << di)) != 0;
		if (on) {
			float x = (di) * grid;
			float y = 0; //(si * grid * 8) + (di * grid);
			glVertex2f(x + size, y + size);
			glVertex2f(x, y + size);
			glVertex2f(x, y);
			glVertex2f(x + size, y);
		}
	}
#endif
    glEnd();
    glutSwapBuffers();
    //glFlush();				/* Complete any pending operations */
}

void keyCB(unsigned char key, int x, int y)	/* called on key press */
{
	if (key == 'q')
		exit(0);
	//static uint8_t buf[64];
	switch (key) {
		case 'q':
		case 0x1f: // escape
			exit(0);
			break;
		case ' ':
			do_button_press++; // pass the message to the AVR thread
			break;
		case 'r':
			printf("Starting VCD trace\n");
			avr_vcd_start(&vcd_file);
			break;
		case 's':
			printf("Stopping VCD trace\n");
			avr_vcd_stop(&vcd_file);
			break;
	}
}

// gl timer. if the pin have changed states, refresh display
void timerCB(int i)
{
	//static uint8_t oldstate = 0xff;
	// restart timer
	glutTimerFunc(1000/64, timerCB, 0);
#if 0
	if (oldstate != pin_state) {
		oldstate = pin_state;
		glutPostRedisplay();
	}
#endif
}

static void * avr_run_thread(void * oaram)
{
//	int b_press = do_button_press;

	while (1) {
		int state = avr_run(avr);
		if ( state == cpu_Done || state == cpu_Crashed)
			break;
	}
	return NULL;
}


char avr_flash_path[1024];
int avr_flash_fd = 0;

// avr special flash initalization
// here: open and map a file to enable a persistent storage for the flash memory
void avr_special_init( avr_t * avr)
{
	// open the file
	avr_flash_fd = open(avr_flash_path, O_RDWR|O_CREAT, 0644);
	if (avr_flash_fd < 0) {
		perror(avr_flash_path);
		exit(1);
	}
	// resize and map the file the file
	(void)ftruncate(avr_flash_fd, avr->flashend + 1);
	ssize_t r = read(avr_flash_fd, avr->flash, avr->flashend + 1);
	if (r != avr->flashend + 1) {
		fprintf(stderr, "unable to load flash memory\n");
		perror(avr_flash_path);
		exit(1);
	}
}

// avr special flash deinitalization
// here: cleanup the persistent storage
void avr_special_deinit( avr_t* avr)
{
	puts(__func__);
	lseek(avr_flash_fd, SEEK_SET, 0);
	ssize_t r = write(avr_flash_fd, avr->flash, avr->flashend + 1);
	if (r != avr->flashend + 1) {
		fprintf(stderr, "unable to load flash memory\n");
		perror(avr_flash_path);
	}
	close(avr_flash_fd);
	uart_pty_stop(&uart_pty);
}

int main(int argc, char *argv[])
{
	int debug = 0;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-d"))
			debug++;
	avr = avr_make_mcu_by_name("atmega644");
	if (!avr) {
		fprintf(stderr, "%s: Error creating the AVR core\n", argv[0]);
		exit(1);
	}
//	snprintf(avr_flash_path, sizeof(avr_flash_path), "%s/%s", pwd, "simduino_flash.bin");
	strcpy(avr_flash_path,  "reprap_flash.bin");
	// register our own functions
	avr->special_init = avr_special_init;
	avr->special_deinit = avr_special_deinit;
	avr_init(avr);
	avr->frequency = 20000000;
	avr->aref = avr->avcc = avr->vcc = 5 * 1000;	// needed for ADC

	if (1) {
		elf_firmware_t f;
		const char * fname = "/opt/reprap/tvrrug/Marlin.base/Marlin/applet/Marlin.elf";
		elf_read_firmware(fname, &f);

		printf("firmware %s f=%d mmcu=%s\n", fname, (int)f.frequency, f.mmcu);
		avr_load_firmware(avr, &f);
	} else {
		char path[1024];
		uint32_t base, size;
//		snprintf(path, sizeof(path), "%s/%s", pwd, "ATmegaBOOT_168_atmega328.ihex");
		strcpy(path, "marlin/Marlin.hex");
		//strcpy(path, "marlin/bootloader-644-20MHz.hex");
		uint8_t * boot = read_ihex_file(path, &size, &base);
		if (!boot) {
			fprintf(stderr, "%s: Unable to load %s\n", argv[0], path);
			exit(1);
		}
		printf("Firmware %04x: %d\n", base, size);
		memcpy(avr->flash + base, boot, size);
		free(boot);
		avr->pc = base;
		avr->codeend = avr->flashend;
	}
	//avr->trace = 1;

	// even if not setup at startup, activate gdb if crashing
	avr->gdb_port = 1234;
	if (debug) {
		printf("AVR is stopped, waiting on gdb on port %d. Use 'target remote :%d' in avr-gdb\n",
				avr->gdb_port, avr->gdb_port);
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}

	uart_pty_init(avr, &uart_pty);
	uart_pty_connect(&uart_pty, '0');

	thermistor_init(avr, &therm_hotend, 0,
			(short*)temptable_5, sizeof(temptable_5) / sizeof(short) / 2, OVERSAMPLENR, 25.0f);
	thermistor_init(avr, &therm_hotbed, 2,
			(short*)temptable_5, sizeof(temptable_5) / sizeof(short) / 2, OVERSAMPLENR, 30.0f);
	thermistor_init(avr, &therm_spare, 1,
			(short*)temptable_5, sizeof(temptable_5) / sizeof(short) / 2, OVERSAMPLENR, 10.0f);

	/*
	 * OpenGL init, can be ignored
	 */
	glutInit(&argc, argv);		/* initialize GLUT system */

	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutInitWindowSize(8 * pixsize, 1 * pixsize);		/* width=400pixels height=500pixels */
	window = glutCreateWindow("Glut");	/* create window */

	// Set up projection matrix
	glMatrixMode(GL_PROJECTION); // Select projection matrix
	glLoadIdentity(); // Start with an identity matrix
	glOrtho(0, 8 * pixsize, 0, 1 * pixsize, 0, 10);
	glScalef(1,-1,1);
	glTranslatef(0, -1 * pixsize, 0);

	glutDisplayFunc(displayCB);		/* set window's display callback */
	glutKeyboardFunc(keyCB);		/* set window's key callback */
	glutTimerFunc(1000 / 24, timerCB, 0);

	// the AVR run on it's own thread. it even allows for debugging!
	pthread_t run;
	pthread_create(&run, NULL, avr_run_thread, NULL);

	glutMainLoop();
}
