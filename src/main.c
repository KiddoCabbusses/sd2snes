#include <arm/NXP/LPC17xx/LPC17xx.h>
#include <string.h>
#include "config.h"
#include "obj/autoconf.h"
#include "clock.h"
#include "uart.h"
#include "bits.h"
#include "power.h"
#include "timer.h"
#include "ff.h"
#include "diskio.h"
#include "spi.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "filetypes.h"
#include "memory.h"
#include "snes.h"
#include "led.h"
#include "sort.h"
#include "cic.h"
#include "tests.h"
#include "cli.h"
#include "sdnative.h"
#include "crc.h"
#include "smc.h"
#include "msu1.h"

#include "usb_hid.h"
#define EMC0TOGGLE	(3<<4)
#define MR0R		(1<<1)

int i;

int sd_offload = 0, ff_sd_offload = 0, sd_offload_tgt = 0;
int sd_offload_partial = 0;
uint16_t sd_offload_partial_start = 0;
uint16_t sd_offload_partial_end = 0;

/* FIXME HACK */
volatile enum diskstates disk_state;
extern volatile tick_t ticks;
extern snes_romprops_t romprops;

int main(void) {
  LPC_GPIO2->FIODIR = BV(0) | BV(1) | BV(2);
  LPC_GPIO1->FIODIR = 0;
  LPC_GPIO0->FIODIR = BV(16);

 /* connect UART3 on P0[25:26] + SSP0 on P0[15:18] + MAT3.0 on P0[10] */
  LPC_PINCON->PINSEL1 = BV(18) | BV(19) | BV(20) | BV(21) /* UART3 */
                      | BV(3) | BV(5);                    /* SSP0 (FPGA) except SS */
  LPC_PINCON->PINSEL0 = BV(31);                            /* SSP0 */
/*                      | BV(13) | BV(15) | BV(17) | BV(19)  SSP1 (SD) */

 /* pull-down CIC data lines */
  LPC_PINCON->PINMODE3 = BV(18) | BV(19) | BV(20) | BV(21);

  clock_disconnect();
  snes_init();
  snes_reset(1);
  power_init();
  timer_init();
  uart_init();
  fpga_spi_init();
  spi_preinit();
  led_init();
 /* do this last because the peripheral init()s change PCLK dividers */
  clock_init();
  LPC_PINCON->PINSEL0 |= BV(20) | BV(21);                  /* MAT3.0 (FPGA clock) */
led_pwm();
  sdn_init();
  printf("\n\nsd2snes mk.2\n============\nfw ver.: " VER "\ncpu clock: %d Hz\n", CONFIG_CPU_FREQUENCY);
printf("PCONP=%lx\n", LPC_SC->PCONP);
  file_init();
  cic_init(0);
/* setup timer (fpga clk) */
  LPC_TIM3->CTCR=0;
  LPC_TIM3->EMR=EMC0TOGGLE;
  LPC_TIM3->MCR=MR0R;
  LPC_TIM3->MR0=1;
  LPC_TIM3->TCR=1;
  fpga_init();
  fpga_rompgm();
  sram_writebyte(0, SRAM_CMD_ADDR);
  while(1) {
    set_mcu_ovr(1);
    if(disk_state == DISK_CHANGED) {
      sdn_init();
      newcard = 1;
    }
    load_bootrle(SRAM_MENU_ADDR);
    set_saveram_mask(0x1fff);
    set_rom_mask(0x3fffff);
    set_mapper(0x7);
    set_mcu_ovr(0);
    snes_reset(0);
    while(get_cic_state() == CIC_FAIL) {
      rdyled(0);
      readled(0);
      writeled(0);
      delay_ms(500);
      rdyled(1);
      readled(1);
      writeled(1);
      delay_ms(500);
    }
    /* some sanity checks */
    uint8_t card_go = 0;
    while(!card_go) {
      if(disk_status(0) & (STA_NOINIT|STA_NODISK)) {
	snes_bootprint("            No Card!            \0");
	while(disk_status(0) & (STA_NOINIT|STA_NODISK));
	delay_ms(200);
      }
      file_open((uint8_t*)"/sd2snes/menu.bin", FA_READ);
      if(file_status != FILE_OK) {
	snes_bootprint("  /sd2snes/menu.bin not found!  \0");
	while(disk_status(0) == RES_OK);
      } else {
	card_go = 1;
      }
      file_close();
    }
    snes_bootprint("           Loading ...          \0");
    if(get_cic_state() == CIC_PAIR) {
      printf("PAIR MODE ENGAGED!\n");
      cic_pair(CIC_PAL, CIC_NTSC);
    }
    rdyled(1);
    readled(0);
    writeled(0);
    /* exclusive mode */
    set_mcu_ovr(1);

    *fs_path=0;
    uint32_t saved_dir_id;
    get_db_id(&saved_dir_id);

    uint32_t mem_dir_id = sram_readlong(SRAM_DIRID);
    uint32_t mem_magic = sram_readlong(SRAM_SCRATCHPAD);
    printf("mem_magic=%lx mem_dir_id=%lx saved_dir_id=%lx\n", mem_magic, mem_dir_id, saved_dir_id);
    if((mem_magic != 0x12345678) || (mem_dir_id != saved_dir_id) || (newcard)) {
      newcard = 0;
      /* generate fs footprint (interesting files only) */
      uint32_t curr_dir_id = scan_dir(fs_path, 0, 0);
      printf("curr dir id = %lx\n", curr_dir_id);
      /* files changed or no database found? */
      if((get_db_id(&saved_dir_id) != FR_OK)
	|| saved_dir_id != curr_dir_id) {
	/* rebuild database */
	printf("saved dir id = %lx\n", saved_dir_id);
	printf("rebuilding database...");
	snes_bootprint("     rebuilding database ...    \0");
	curr_dir_id = scan_dir(fs_path, 1, 0);
	sram_writeblock(&curr_dir_id, SRAM_DB_ADDR, 4);
	uint32_t endaddr, direndaddr;
	sram_readblock(&endaddr, SRAM_DB_ADDR+4, 4);
	sram_readblock(&direndaddr, SRAM_DB_ADDR+8, 4);
	printf("%lx %lx\n", endaddr, direndaddr);
	printf("sorting database...");
	snes_bootprint("       sorting database ...     \0");
	sort_all_dir(direndaddr);
	printf("done\n");
	snes_bootprint("        saving database ...     \0");
	save_sram((uint8_t*)"/sd2snes/sd2snes.db", endaddr-SRAM_DB_ADDR, SRAM_DB_ADDR);
	save_sram((uint8_t*)"/sd2snes/sd2snes.dir", direndaddr-(SRAM_DIR_ADDR), SRAM_DIR_ADDR);
	printf("done\n");
      } else {
	printf("saved dir id = %lx\n", saved_dir_id);
	printf("different card, consistent db, loading db...\n");
	load_sram((uint8_t*)"/sd2snes/sd2snes.db", SRAM_DB_ADDR);
	load_sram((uint8_t*)"/sd2snes/sd2snes.dir", SRAM_DIR_ADDR);
      }
      sram_writelong(curr_dir_id, SRAM_DIRID);
      sram_writelong(0x12345678, SRAM_SCRATCHPAD);
    } else {
      printf("same card, loading db...\n");
      load_sram((uint8_t*)"/sd2snes/sd2snes.db", SRAM_DB_ADDR);
      load_sram((uint8_t*)"/sd2snes/sd2snes.dir", SRAM_DIR_ADDR);
    }
    /* load menu */
    fpga_pgm((uint8_t*)"/main.bit.rle");

    uart_putc('(');
    load_rom((uint8_t*)"/sd2snes/menu.bin", SRAM_MENU_ADDR);
    /* force memory size + mapper */
    set_rom_mask(0x3fffff);
    set_mapper(0x7);
    uart_putc(')');
    uart_putcrlf();

    sram_writebyte(0, SRAM_CMD_ADDR);

    /* shared mode */
    set_mcu_ovr(0);

    printf("SNES GO!\n");
    snes_reset(1);
    delay_ms(1);
    snes_reset(0);

    uint8_t cmd = 0;
    uint32_t filesize=0;
    sram_writebyte(32, SRAM_CMD_ADDR);
    printf("test sram\n");
    while(!sram_reliable());
    printf("ok\n");

  //while(1) {
  //  delay_ms(1000);
  //  printf("Estimated SNES master clock: %ld Hz\n", get_snes_sysclk());
  //}
  //sram_hexdump(SRAM_DB_ADDR, 0x200);
  //sram_hexdump(SRAM_MENU_ADDR, 0x400);
    while(!cmd) {
      cmd=menu_main_loop();
// cmd = 1;
      printf("cmd: %d\n", cmd);
      sleep_ms(50);
      uart_putc('-');
      switch(cmd) {
	case SNES_CMD_LOADROM:
	  get_selected_name(file_lfn);
	  set_mcu_ovr(1);
// strcpy((char*)file_lfn, "/roms/b/BS Zelda no Densetsu Kodai no Sekiban Dai 1 Hanashi (J).smc");
	  printf("Selected name: %s\n", file_lfn);
	  filesize = load_rom(file_lfn, SRAM_ROM_ADDR);
	  if(romprops.ramsize_bytes) {
	    strcpy(strrchr((char*)file_lfn, (int)'.'), ".srm");
	    printf("SRM file: %s\n", file_lfn);
	    load_sram(file_lfn, SRAM_SAVE_ADDR);
	  } else {
	    printf("No SRAM\n");
	  }
	  set_mcu_ovr(0);
	  snes_reset(1);
	  delay_ms(10);
	  snes_reset(0);
	  break;
	case SNES_CMD_SETRTC:
	  cmd=0; /* stay in loop */
	  break;
	default:
	  printf("unknown cmd: %d\n", cmd);
	  cmd=0; /* unknown cmd: stay in loop */
	  break;
      }
    }
    printf("cmd was %x, going to snes main loop\n", cmd);

    /* always try MSU1 for now */
    if(msu1_entrycheck_and_loop()) {
      prepare_reset();
      continue;
    }

    cmd=0;
    uint8_t snes_reset_prev=0, snes_reset_now=0, snes_reset_state=0;
    uint16_t reset_count=0;
    while(fpga_test() == FPGA_TEST_TOKEN) {
      cli_entrycheck();
      sleep_ms(250);
      sram_reliable();
      printf("%s ", get_cic_statename(get_cic_state()));
      snes_reset_now=get_snes_reset();
      if(snes_reset_now) {
	if(!snes_reset_prev) {
	  printf("RESET BUTTON DOWN\n");
	  snes_reset_state=1;
	  reset_count=0;
	}
      } else {
	if(snes_reset_prev) {
	  printf("RESET BUTTON UP\n");
	  snes_reset_state=0;
	}
      }
      if(snes_reset_state) {
	reset_count++;
      } else {
	sram_reliable();
	snes_main_loop();
      }
      if(reset_count>4) {
	reset_count=0;
        prepare_reset();
	break;
      }
      snes_reset_prev = snes_reset_now;
    }
    /* fpga test fail: panic */
    if(fpga_test() != FPGA_TEST_TOKEN){
      led_panic();
    }
    /* else reset */
  }
}

