/* Very primitive emulator of Commodore 65 + sub-set (!!) of Mega65 fetures.
   Copyright (C)2016 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "xemu/emutools.h"
#include "xemu/f018_core.h"


/* NOTES ABOUT C65/M65 DMAgic "F018", AND THIS EMULATION:

	* this emulation part uses "callbacks" and it depends on the target emulator how to implement read/write ops by DMA!
	* modulo currently not handled, and it seems there is not so much decent specification how it does work
	* INT / interrupt is not handled
	* MIX command is implemented, though without decent specification is more like a guessing only
	* SWAP command is also a guessing, but a better one, as it's kinda logical what it is used for
	* COPY/FILL should be OK
	* DMA length of ZERO probably means $10000 [but not so much info on this ...] FIXME?
	* Current emulation timing is incorrect
	* Speed of DMA in a real C65 would be affected by many factors, kind of op, or even VIC-3 fetchings etc ... "Of course" it's not emulated ...
	* DMA sees the upper memory regions (ie in BANK 4 and above), which may have "something", though REC (Ram Expansion Controller) is not implemented yet
	* It's currently unknown that DMA registers are modified as DMA list is read, ie what happens if only reg 0 is re-written
	* C65 specification tells about "not implemented sub-commands": I am curious what "sub commands" are or planned as, etc ...
	* Reading status would resume interrupted DMA operation (?) it's not emulated
	* MEGA65 macro is defined in case of M65 target, then we handle the "megabyte slice stuff" as well.
*/

#define DMA_MAX_ITERATIONS	(256*1024)


Uint8 dma_registers[16];		// The four DMA registers (with last values written by the CPU)
int   dma_chip_revision;		// revision of DMA chip
static int   source_step;		// [-1, 0, 1] step value for source (0 = hold, constant address)
#ifdef MEGA65
static int   source_step_fraction;      // [1/256] step value for source (0x100 = no fraction)
static int   source_step_remain;        // fractional reaminder for the next DMA-step
#endif
static int   target_step;		// [-1, 0, 1] step value for target (0 = hold, constant address)
#ifdef MEGA65
static int   target_step_fraction;      // [1/256] step value for source (0x100 = no fraction)
static int   target_step_remain;        // fractional reaminder for the next DMA-step
#endif
static int   source_addr;		// DMA source address (the low byte is also used by COPY command as the "filler byte")
static int   target_addr;		// DMA target address
static int   source_is_io;		// DMA source is I/O space (only the lower 12 bits are used of the source_addr then?)
static int   target_is_io;		// DMA target is I/O space (only the lower 12 bits are used of the target_addr then?)
static int   modulo;			// Currently not used/emulated
static int   source_uses_modulo;	// Currently not used/emulated
static int   target_uses_modulo;	// Currently not used/emulated
static int   length;			// DMA operation length
static int   command;			// DMA command
static int   chained;			// 1 = chained (read next DMA operation "descriptor")
static int   dma_list_addr;		// Current address of the DMA list, controller will read to "execute"
static Uint8 minterms[4];		// Used with MIX DMA command only
       Uint8 dma_status;
//static int   mb_list, mb_source, mb_target;
#ifdef MEGA65
static int   transparency_on=0;           // DMA uses tranperency byte
static int   transparency_val=0;          // DMA      tranperency byte
static int   extended_list=0;          // DMA     using the Extendedlist
#endif

// These are call-backs, emulator is initialized with, and the set, a DMA operation can choose from, see below
// See dma_init()
// Source/target is separated, as the emulator may implement phys addr mapping cache, so it can exploit the
// likely situation that the next byte to read/write is within the same 256 byte page, so there is no need
// for a full address decoding "circle" all the time ...
static dma_reader_cb_t cb_source_mreader;
static dma_writer_cb_t cb_source_mwriter;
static dma_reader_cb_t cb_target_mreader;
static dma_writer_cb_t cb_target_mwriter;
static dma_reader_cb_t cb_source_ioreader;
static dma_writer_cb_t cb_source_iowriter;
static dma_reader_cb_t cb_target_ioreader;
static dma_writer_cb_t cb_target_iowriter;
// These are call-backs, assigned by the DMA operation initiaiter
static dma_reader_cb_t source_reader;
static dma_reader_cb_t target_reader;
static dma_reader_cb_t list_reader;
static dma_writer_cb_t source_writer;
static dma_writer_cb_t target_writer;
// ...
static int source_mask, target_mask, source_megabyte, target_megabyte, list_megabyte;
static int source_cur_megabyte=0, target_cur_megabyte=0;
static int dma_phys_io_offset, dma_phys_io_offset_default = 0;

int calc_fractional_step(int step, int fraction, int * remainder){
  
        long int temp_step=step;

        temp_step  *= fraction;
        *remainder += temp_step%0x100;  // remember the fractional part for the next step
        temp_step  /= 0x100;
        temp_step  += (*remainder)/256;
        *remainder &= 255;
  
        return (temp_step);
}



// TODO: modulo?
static INLINE Uint8 read_source_next ( void )
{
	// We use "+" operator for "megabyte" (see the similar functions below too)
	// Since, if it is I/O source/target, then it's allowed the emulator specify
	// a physical address for I/O which also contains an offset within the "mbyte slice"
	// range as well (ie, mega65 uses I/O areas mapped in the $FF megabyte area, for
	// various I/O modes)
        Uint8 result = source_reader((source_addr & source_mask) + source_cur_megabyte);

#ifdef MEGA65
        source_addr += calc_fractional_step(source_step,source_step_fraction,&source_step_remain);
#else        
        source_addr += source_step;
#endif        
	return result;
}


// TODO: modulo?
static INLINE void write_target_next ( Uint8 data )
{
	// See the comment at read_source_next()
#ifdef MEGA65
        if ((!transparency_on) || (transparency_val != data))
#endif
            target_writer((target_addr & target_mask) + target_cur_megabyte, data);
#ifdef MEGA65
        target_addr += calc_fractional_step(target_step,target_step_fraction,&target_step_remain);
#else
        target_addr += target_step;
#endif
}


// TODO: modulo?
static INLINE void swap_next ( void )
{
	// See the comment at read_source_next()
	Uint8 sa = source_reader((source_addr & source_mask) + source_cur_megabyte);
	Uint8 da = target_reader((target_addr & target_mask) + target_cur_megabyte);
#ifdef MEGA65
        if ((!transparency_on) || (transparency_val != sa) || (transparency_val != da))
#endif
        {
            source_writer((source_addr & source_mask) + source_cur_megabyte, da);
            target_writer((target_addr & target_mask) + target_cur_megabyte, sa);
        }
	source_addr += source_step;
	target_addr += target_step;
}


// TODO: modulo?
static INLINE void mix_next ( void )
{
	// See the comment at read_source_next()
	Uint8 sa = source_reader((source_addr & source_mask) + source_cur_megabyte);
	Uint8 da = target_reader((target_addr & target_mask) + target_cur_megabyte);
	// NOTE: it's not clear from the specification, what MIX
	// does. I assume, that it does some kind of minterm
	// with source and target and writes the result to
	// target. I'm not even sure how the minterms are
	// interpreted on the bits of two bytes too much. FIXME!!!
	da =
		(( sa) & ( da) & minterms[3]) |
		(( sa) & (~da) & minterms[2]) |
		((~sa) & ( da) & minterms[1]) |
		((~sa) & (~da) & minterms[0]) ;
	// See the comment at read_source_next()
#ifdef MEGA65
        if ((!transparency_on) || (transparency_val != da))
#endif
            target_writer((target_addr & target_mask) + target_cur_megabyte, da);
	source_addr += source_step;
	target_addr += target_step;	
}



static INLINE Uint8 read_dma_list_next ( void )
{
	// Unlike the functions above, DMA list read is always memory (not I/O)
	// Also the "step" is always one. So it's a bit special case 
	return list_reader(((dma_list_addr++) & 0xFFFFF) + list_megabyte);
}


static void dma_update_all ( void )
{
	int limit = DMA_MAX_ITERATIONS;
	while (dma_status) {
		dma_update();
		if (!limit--)
			FATAL("FATAL: Run-away DMA session, still blocking the emulator after %d iterations, exiting!", DMA_MAX_ITERATIONS);
	}
}

#ifdef MEGA65
void dma_extended_list( void ){

            Uint8 list_val;
 
            DEBUG("DMA: Extendedlist fetching at Listaddres is [MB=$%02X]$%06X \n" NL, list_megabyte >> 20, dma_list_addr);
#if 0
                // Ugly workaround for outdated kickstart, it still uses d705 for Source Megabyte and d706 for Target Megabyte
            if (dma_list_addr+list_megabyte==0xfff0000){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
            if (dma_list_addr+list_megabyte==0xfff9aff){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
            if (dma_list_addr+list_megabyte==0xfff9400){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
            if (dma_list_addr+list_megabyte==0xfff9b00){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
            if (dma_list_addr+list_megabyte==0xfff9a00){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
            if (dma_list_addr+list_megabyte==0xfffa800){
                source_megabyte=(int)dma_registers[5] << 20;
                return;
            }
#endif
            do{
              list_val=read_dma_list_next();
              DEBUG("DMA: Extendedlist List value is $%02X now\n", list_val);
              switch(list_val){
                  case 0x0a:  // Request Format is Revison A
                      dma_chip_revision=0;
                      break;
                  case 0x0b:  // Request Format is Revison B
                      dma_chip_revision=1;
                      break;
                  case 0x80:  // Set MB of source address
                      source_megabyte=read_dma_list_next();
                      break;
                  case 0x81:  // Set MB of source address
                      target_megabyte=read_dma_list_next(); 
                      break;
                  case 0x82:  // Set lowerbyte of source fractional stepping
                      source_step_fraction&=0xff00;
                      source_step_fraction|=read_dma_list_next();
                      break;
                  case 0x83:  // Set higherbyte of source fractional stepping
                      source_step_fraction&=0x00ff;
                      source_step_fraction|=(read_dma_list_next())<<8;
                      break;
                  case 0x84:  // Set lowerbyte of target fractional stepping
                      target_step_fraction&=0xff00;
                      target_step_fraction|=read_dma_list_next();
                      break;
                  case 0x85:  // Set higherbyte of source fractional stepping
                      target_step_fraction&=0x00ff;
                      target_step_fraction|=(read_dma_list_next())<<8;
                      break;
                  case 0x06:  // switch on transperency mode (don't write source bytes to destination, if byte value matches $xx)
                      transparency_on=1;
                      break;
                  case 0x07:  // switch off transperency mode
                      transparency_on=0;
                      break;
                  case 0x86:  // Set higherbyte of source fractional stepping
                      transparency_val=(read_dma_list_next());
                      break;
                  case 0x00:  // This ends Extended-DMA-List
                      break; 
                  case 0x8d:  // Whatever 
                      DEBUG("DMA reads 8d 0x%02x%02x\n",read_dma_list_next(),read_dma_list_next());
                      break;
                  default:
                      /* Something is going wrong here. Either List adress was false, or values are mixed up
                       * Better not doing the DMA */
                      FATAL("Unexpected Value 0x%02x in extended DMA-List at 0x%02x%04x",list_val,list_megabyte >>20,dma_list_addr);

                      return;
                      break;
              }
            }while(list_val);   // a 0 will end the list
        DEBUG("DMA: Extendedlist finished Listaddres is [MB=$%02X]$%06X now" NL, list_megabyte >> 20, dma_list_addr);


}
#endif
void dma_write_reg ( int addr, Uint8 data )
{
	// DUNNO about DMAgic too much. It's merely guessing from my own ROM assembly tries, C65gs/Mega65 VHDL, and my ideas :)
	// The following condition is commented out for now. FIXME: how it is handled for real?!
	//if (vic_iomode != VIC4_IOMODE)
	//	addr &= 3;
        DEBUG ("Write DMA Reg 0x%02x with 0x%02x \n", addr ,data);
#ifdef MEGA65
	dma_registers[addr] = data;
	switch (addr) {
		case 0x2:	// for compatibility with C65, Mega65 here resets the MB part of the DMA list address
			dma_registers[4] = 0;	// this is the "MB" part of the DMA list address (hopefully ...)
			break;
                case 0xE:       // Set low order bits of DMA list address, without starting (Mega65 feature, but without VIC4 new mode, this reg will never addressed here anyway)
                        dma_registers[0] = data;
                        break;
	}
#else
	if (addr > 3) {	// in case of C65, the extended registers cannot be written
		DEBUG("DMA: trying to write M65-specific DMA register (%d) with a C65 ..." NL, addr);
		return;
	}
	dma_registers[addr] = data;
#endif
#ifdef MEGA65
        if ((addr!=0x00)&&(addr!=0x05))
#else
	if (addr)
#endif
		return;	// Only writing register 0 starts the DMA operation, otherwise just return from this function (reg write already happened)
	if (dma_status) {
		DEBUG("DMA: WARNING: previous operation is in progress, WORKAROUND: finishing first." NL);
		// Ugly hack: it seems even the C65 ROM issues new DMA commands while the previous is in-progress
		// It's possible the fault of timing of my emulation.
		// The current workaround: in this situation we run the DMA to finish the previous operation first.
		// Note, that there is a possible two PROBLEMS with this solution:
		// * Extremly long DMA command (ie chained) blocks the emulator to refresh screen etc for a long time
		// * I/O redirection as target affecting the DMA registers can create a stack overflow in the emulator code :)
		dma_update_all();
	}
#ifdef MEGA65
        /* Write to Register D705 starts DMA with extended DMA-list 
         * It has features like fractional stepping or transparency-mode 
         */
        list_megabyte = (int)dma_registers[4] << 20;    // the "MB" part to select MegaByte range for the DMA list reading
        if (addr==0x00){
            command = -1;           // signal dma_update() that it's needed to fetch the DMA command, no command is fetched yet
            extended_list=0;
            dma_list_addr = dma_registers[0] | (dma_registers[1] << 8) | ((dma_registers[2] & 15) << 16);
            DEBUG("DMA: Listaddres is [MB=$%02X]$%06X now" NL, list_megabyte >> 20, dma_list_addr);
        }
        if (addr==0x05){ 
            if (data == 0){    // ugly workaround for using d705 as megabyte offset
                return;
            }
            command = -2;           // signal dma_update() that it's needed to fetch the DMA extended list , no command is fetched yet
            extended_list=1;
            dma_list_addr = dma_registers[5] | (dma_registers[1] << 8) | ((dma_registers[2] & 15) << 16);
            DEBUG("DMA: Extendedlist Listaddres is [MB=$%02X]$%06X now" NL, list_megabyte >> 20, dma_list_addr);
        }
#else	
        dma_list_addr = dma_registers[0] | (dma_registers[1] << 8) | ((dma_registers[2] & 15) << 16);
        command = -1;           // signal dma_update() that it's needed to fetch the DMA command, no command is fetched yet
#endif
	DEBUG("DMA: list address is [MB=$%02X]$%06X now, just written to register %d value $%02X" NL, list_megabyte >> 20, dma_list_addr, addr, data);
	dma_status = 0x80;	// DMA is busy now, also to signal the emulator core to call dma_update() in its main loop
	dma_update_all();	// DMA _stops_ CPU, however FIXME: interrupts can (???) occur, so we need to emulate that somehow later?
}



/* Main emulation loop should call this function regularly, if dma_status is not zero.
   This way we have 'real' DMA, ie works while the rest of the machine is emulated too.
   Please note, that the "exact" timing of DMA and eg the CPU is still incorrect, but it's far
   better than the previous version where DMA was "blocky", ie the whole machine was "halted" while DMA worked ...
   ---------------------------------------------------------------------------------------------------------------
   OK, now it seems, there are (at least?) two major (or planned?) DMA revisions in C65 with many incompatibilities.
   Let's call them "A" and "B" (F018A and F018B). Currently, Xemu decides according the variable "dma_chip_revision" being 0
   means "A" other non-zero values are "B", set by dma_init() function called by the emulator.
   With MEGA65, some extra features have been added, we call this revision F018A+

   Command byte:   bits 0,1 -> DMA command
                   bit  2   -> chained bit
		   bit  3   -> interrupt? [not emulated!]
		   F018A:
			bit 4,5,6,7 = MIX minterm selection
		   F018B:
			bit4: source direction
			bit5: target direction
			bit6: ?
			bit7: ?

   Source/target MS byte:
		   bits 0-3: -> address bits   19,18,17,16
                   F018A:
			bit 4:  HOLD
			bit 5:  MOD
			bit 6:  DIR
			bit 7:  IO
                   F018B:
			no idea ... maybe 1Mb bank selection?

   Extra byte in DMA list fetch before modulo (CALLED: "subcommand" also?):
		ONLY IN CASE OF F016B (12 bytes / DMA command, for F018A it's only 11 bytes ...)
*/
void dma_update ( void )
{
	Uint8 subcommand;
	if (!dma_status)
		return;
#ifdef MEGA65
        if (command == -2) {
            dma_extended_list();
            command = -1;
        }
#endif
	if (command == -1) {
		// command == -1 signals the situation, that the (next) DMA command should be read!
		// This part is highly incorrect, ie fetching so many bytes in one step only of dma_update()
		command      = read_dma_list_next()      ;
		length       = read_dma_list_next()      ;
		length      |= read_dma_list_next() <<  8;
		source_addr  = read_dma_list_next()      ;
		source_addr |= read_dma_list_next() <<  8;
		source_addr |= read_dma_list_next() << 16;
		target_addr  = read_dma_list_next()      ;
		target_addr |= read_dma_list_next() <<  8;
		target_addr |= read_dma_list_next() << 16;
		if (dma_chip_revision)	// for F018B we have an extra byte fetch here! used later in this function
			subcommand = read_dma_list_next();
		modulo       = read_dma_list_next()      ;	// modulo is not so much handled yet, maybe it's not even a 16 bit value
		modulo      |= read_dma_list_next() <<  8;	// ... however since it's currently not used, it does not matter too much
                DEBUG("DMA: list content, command:0x%02x length:0x%04x source:0x%02x%04x target:0x%02x%04x modulo:0x%02x" NL,command,length,source_megabyte,source_addr,target_megabyte,target_addr,modulo );
		
		if (dma_chip_revision) {
			// F018B ("new") behaviour
			source_step  = (command & 16) ? -1 : 1;
			target_step  = (command & 32) ? -1 : 1;
			//source_uses_modulo =
			//target_uses_modulo =
			// FIXME: what about minterms in F018B?!
		} else {
			// F018A ("old") behaviour
			source_step  = (source_addr & 0x100000) ? 0 : ((source_addr & 0x400000) ? -1 : 1);
                        target_step  = (target_addr & 0x100000) ? 0 : ((target_addr & 0x400000) ? -1 : 1);
			source_uses_modulo = (source_addr & 0x200000);
			target_uses_modulo = (target_addr & 0x200000);
			minterms[0] = (command &  16) ? 0xFF : 0x00;
			minterms[1] = (command &  32) ? 0xFF : 0x00;
			minterms[2] = (command &  64) ? 0xFF : 0x00;
			minterms[3] = (command & 128) ? 0xFF : 0x00;
		}
		// It *seems* I/O stuff is still in the place even with F018B. FIXME: is it true?
		source_is_io = (source_addr & 0x800000);
		target_is_io = (target_addr & 0x800000);
		// FIXME: for F018B, we should allow "1mbyte bank" selection!!!!!!!
		source_addr &= 0xFFFFF;	// C65 1-mbyte range, chop bits used for other purposes off
		target_addr &= 0xFFFFF; // C65 1-mbyte range, chop bits used for other purposes off
		/* source selection */
		if (source_is_io) {
			source_reader	= cb_source_ioreader;
			source_writer	= cb_source_iowriter;
			source_mask	= 0xFFF;	// 4K I/O size
			source_cur_megabyte	= dma_phys_io_offset;
                        DEBUG("DMA: source  IO, command:0x%02x length:0x%04x source:0x%06x target:0x%06x modulo:0x%02x" NL,command,length,source_megabyte+source_addr,target_cur_megabyte+target_addr,modulo );
		} else {
			source_reader	= cb_source_mreader;
			source_writer	= cb_source_mwriter;
			source_mask	= 0xFFFFF;	// 1Mbyte of "Mbyte slice" size
			source_cur_megabyte = (source_megabyte<<20);
                        DEBUG("DMA: source  MEM, command:0x%02x length:0x%04x source:0x%06x target:0x%06x modulo:0x%02x" NL,command,length,source_megabyte+source_addr,target_cur_megabyte+target_addr,modulo );
		}
		/* target selection */
		if (target_is_io) {
			target_reader	= cb_target_ioreader;
			target_writer	= cb_target_iowriter;
			target_mask	= 0xFFF;	// 4K I/O size
			target_cur_megabyte	= dma_phys_io_offset;
                        DEBUG("DMA: target IO, command:0x%02x length:0x%04x source:0x%06x target:0x%06x modulo:0x%02x" NL,command,length,source_megabyte+source_addr,target_cur_megabyte+target_addr,modulo );
		} else {
			target_reader	= cb_target_mreader;
			target_writer	= cb_target_mwriter;
			target_mask	= 0xFFFFF;	// 1Mbyte of "Mbyte slice" size
			target_cur_megabyte = (target_megabyte<<20);
                        DEBUG("DMA: target  MEM, command:0x%02x length:0x%04x source:0x%06x target:0x%06x modulo:0x%02x" NL,command,length,source_megabyte+source_addr,target_cur_megabyte+target_addr,modulo );
		}
		/* other stuff */
		chained = (command & 4);
		DEBUG("DMA: READ COMMAND: $%05X[%s%s %d] -> $%05X[%s%s %d] (L=$%04X) CMD=%d (%s)" NL,
			source_addr, source_is_io ? "I/O" : "MEM", source_uses_modulo ? " MOD" : "", source_step,
			target_addr, target_is_io ? "I/O" : "MEM", target_uses_modulo ? " MOD" : "", target_step,
			length, command, chained ? "chain" : "last"
		);
		if (!length)
			length = 0x10000;			// I *think* length of zero means 64K. Probably it's not true!!
		return;
	}
	// We have valid command to be executed, or continue to execute
	//DEBUG("DMA: EXECUTING: command=%d length=$%04X" NL, command & 3, length);
	switch (command & 3) {
		case 0:			// COPY command
			write_target_next(read_source_next());
			break;
		case 1:			// MIX command
			mix_next();
			break;
		case 2:			// SWAP command
			swap_next();
			break;
		case 3:			// FILL command (SRC LO is the filler byte!)
			write_target_next(source_addr & 0xFF);
			break;
	}
	// Check the situation of end of the operation
	length--;
	if (length <= 0) {
		if (chained) {			// chained?
			DEBUG("DMA: end of operation, but chained!" NL);
			dma_status = 0x81;	// still busy then, with also bit0 set (chained)
#ifdef MEGA65
			if (extended_list){ 
                            command = -2;           // signal for next DMA fetch extended - list
                        }else
#endif
                        command = -1;              // signal for next DMA command fetch
		} else {
			DEBUG("DMA: end of operation, no chained next one." NL);
			dma_status = 0;		// end of DMA command
			command = -1;
#ifdef MEGA65
                        source_step_fraction=0x100;        // Cleanup after DMA is done. Everything is back to initial-state   
                        target_step_fraction=0x100;
                        source_step_remain=0;
                        target_step_remain=0;
                        source_megabyte=0;
                        target_megabyte=0;
                        transparency_on=0;
                        transparency_val=0;

#endif
		}
	}
}


void dma_set_phys_io_offset ( int offs )
{
	dma_phys_io_offset_default = dma_phys_io_offset = offs;
}


void dma_init (
	int dma_rev_set,
	dma_reader_cb_t set_source_mreader , dma_writer_cb_t set_source_mwriter , dma_reader_cb_t set_target_mreader , dma_writer_cb_t set_target_mwriter,
	dma_reader_cb_t set_source_ioreader, dma_writer_cb_t set_source_iowriter, dma_reader_cb_t set_target_ioreader, dma_writer_cb_t set_target_iowriter,
	dma_reader_cb_t set_list_reader
)
{
	dma_chip_revision = dma_rev_set;
	DEBUGPRINT("DMA: initializing DMA engine for chip revision %d" NL, dma_rev_set);
	cb_source_mreader  = set_source_mreader;
	cb_source_mwriter  = set_source_mwriter;
	cb_target_mreader  = set_target_mreader;
	cb_target_mwriter  = set_target_mwriter;
	cb_source_ioreader = set_source_ioreader;
	cb_source_iowriter = set_source_iowriter;
	cb_target_ioreader = set_target_ioreader;
	cb_target_iowriter = set_target_iowriter;
	list_reader = set_list_reader;
	dma_reset();
}


void dma_reset ( void )
{
	command = -1;	// no command is fetched yet
	dma_status = 0;
	memset(dma_registers, 0, sizeof dma_registers);
	source_megabyte = 0;
	target_megabyte = 0;
	list_megabyte = 0;
#ifdef MEGA65
        source_step_fraction=0x100;        // Cleanup after DMA is done. Everything is back to initial-state   
        target_step_fraction=0x100;
        source_step_remain=0;
        target_step_remain=0;
#endif

	dma_phys_io_offset = dma_phys_io_offset_default;
}


Uint8 dma_read_reg ( int addr )
{
	// FIXME: status on ALL registers when read?!
	DEBUG("DMA: register reading at addr of %d" NL, addr);
#if 0
	if ((addr & 3) != 3)
		return 0xFF;	// other registers are (??????) writeonly? FIXME?
#endif
	return dma_status;
}


/* --- SNAPSHOT RELATED --- */

#ifdef XEMU_SNAPSHOT_SUPPORT

// Note: currently state is not saved "within" a DMA operation. It's only a problem, if a DMA
// operation is not handled fully here, but implemented as an iterating update method from the
// emulator code. FIXME.

#include <string.h>

#define SNAPSHOT_DMA_BLOCK_VERSION	1
#define SNAPSHOT_DMA_BLOCK_SIZE		0x100


int dma_snapshot_load_state ( const struct xemu_snapshot_definition_st *def, struct xemu_snapshot_block_st *block )
{
	Uint8 buffer[SNAPSHOT_DMA_BLOCK_SIZE];
	int a;
	if (block->block_version != SNAPSHOT_DMA_BLOCK_VERSION || block->sub_counter || block->sub_size != sizeof buffer)
		RETURN_XSNAPERR_USER("Bad C65 block syntax");
	a = xemusnap_read_file(buffer, sizeof buffer);
	if (a) return a;
	/* loading state ... */
	memcpy(dma_registers, buffer, sizeof dma_registers);
	dma_chip_revision = buffer[0x80];
	return 0;
}


int dma_snapshot_save_state ( const struct xemu_snapshot_definition_st *def )
{
	Uint8 buffer[SNAPSHOT_DMA_BLOCK_SIZE];
	int a = xemusnap_write_block_header(def->idstr, SNAPSHOT_DMA_BLOCK_VERSION);
	if (a) return a;
	memset(buffer, 0xFF, sizeof buffer);
	/* saving state ... */
	memcpy(buffer, dma_registers, sizeof dma_registers);
	buffer[0x80] = dma_chip_revision;
	return xemusnap_write_sub_block(buffer, sizeof buffer);
}

#endif
