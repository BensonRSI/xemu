## Collection of *simple* emulators of some 8 bits machines using SDL2 library,
## including the Commodore LCD and Commodore 65 too.
##
## Copyright (C)2016 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

TARGET		= c65
PRG_TARGET	= xc65

CFLAGS_TARGET_xc65	=
SRCS_TARGET_xc65	= commodore_65.c vic3.c c65_d81_image.c c65_snapshot.c
SRCS_COMMON_xc65	= emutools.c cpu65c02.c cia6526.c sid.c f011_core.c f018_core.c c64_kbd_mapping.c emutools_hid.c cbmhostfs.c emutools_config.c emutools_snapshot.c uart_monitor.c 
CONFIG_CFLAGS_TARGET_xc65	= sdl2|math
CONFIG_LDFLAGS_TARGET_xc65	= sdl2|math

include ../../build/Makefile.common
