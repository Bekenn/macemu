/*
 *  video_vosf.h - Video/graphics emulation, video on SEGV signals support
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef VIDEO_VOSF_H
#define VIDEO_VOSF_H

// Note: this file is #include'd in video_x.cpp
#ifdef ENABLE_VOSF

/*
 *  Page-aligned memory allocation
 */

// Extend size to page boundary
static uint32 page_extend(uint32 size)
{
	const uint32 page_size = getpagesize();
	const uint32 page_mask = page_size - 1;
	return (size + page_mask) & ~page_mask;
}

// Screen fault handler
static bool screen_fault_handler(sigsegv_address_t fault_address, sigsegv_address_t fault_instruction)
{
	D(bug("screen_fault_handler: ADDR=0x%08X from IP=0x%08X\n", fault_address, fault_instruction));
	const uintptr addr = (uintptr)fault_address;
	
	/* Someone attempted to write to the frame buffer. Make it writeable
	 * now so that the data could actually be written. It will be made
	 * read-only back in one of the screen update_*() functions.
	 */
	if ((addr >= mainBuffer.memStart) && (addr < mainBuffer.memEnd)) {
		const int page  = (addr - mainBuffer.memStart) >> mainBuffer.pageBits;
		caddr_t page_ad = (caddr_t)(addr & -mainBuffer.pageSize);
		LOCK_VOSF;
		PFLAG_SET(page);
		vm_protect((char *)page_ad, mainBuffer.pageSize, VM_PAGE_READ | VM_PAGE_WRITE);
		mainBuffer.dirty = true;
		UNLOCK_VOSF;
		return true;
	}
	
	/* Otherwise, we don't know how to handle the fault, let it crash */
	fprintf(stderr, "do_handle_screen_fault: unhandled address 0x%08X", addr);
	if (fault_instruction != SIGSEGV_INVALID_PC)
		fprintf(stderr, " [IP=0x%08X]", fault_instruction);
	fprintf(stderr, "\n");
	return false;
}

/*
 *	Update display for Windowed mode and VOSF
 */

// From video_blit.cpp
extern void (*Screen_blit)(uint8 * dest, const uint8 * source, uint32 length);
extern bool Screen_blitter_init(XVisualInfo * visual_info, bool native_byte_order);

/*	How can we deal with array overrun conditions ?
	
	The state of the framebuffer pages that have been touched are maintained
	in the dirtyPages[] table. That table is (pageCount + 2) bytes long.

Terminology
	
	"Last Page" denotes the pageCount-nth page, i.e. dirtyPages[pageCount - 1].
	"CLEAR Page Guard" refers to the page following the Last Page but is always
	in the CLEAR state. "SET Page Guard" refers to the page following the CLEAR
	Page Guard but is always in the SET state.

Rough process
	
	The update routines must determine which pages have to be blitted to the
	screen. This job consists in finding the first_page that was touched.
	i.e. find the next page that is SET. Then, finding how many pages were
	touched starting from first_page. i.e. find the next page that is CLEAR.

There are two cases to check:

	- Last Page is CLEAR: find_next_page_set() will reach the SET Page Guard
	but it is beyond the valid pageCount value. Therefore, we exit from the
	update routine.
	
	- Last Page is SET: first_page equals (pageCount - 1) and
	find_next_page_clear() will reach the CLEAR Page Guard. We blit the last
	page to the screen. On the next iteration, page equals pageCount and
	find_next_page_set() will reach the SET Page Guard. We still safely exit
	from the update routine because the SET Page Guard position is greater
	than pageCount.
*/

static inline void update_display_window_vosf(void)
{
	int page = 0;
	for (;;) {
		const int first_page = find_next_page_set(page);
		if (first_page >= mainBuffer.pageCount)
			break;

		page = find_next_page_clear(first_page);
		PFLAG_CLEAR_RANGE(first_page, page);

		// Make the dirty pages read-only again
		const int32 offset  = first_page << mainBuffer.pageBits;
		const uint32 length = (page - first_page) << mainBuffer.pageBits;
		vm_protect((char *)mainBuffer.memStart + offset, length, VM_PAGE_READ);
		
		// There is at least one line to update
		const int y1 = mainBuffer.pageInfo[first_page].top;
		const int y2 = mainBuffer.pageInfo[page - 1].bottom;
		const int height = y2 - y1 + 1;
		
		const int bytes_per_row = VideoMonitor.bytes_per_row;
		const int bytes_per_pixel = VideoMonitor.bytes_per_row / VideoMonitor.x;
		int i = y1 * bytes_per_row, j;
		
		if (depth == 1) {

			// Update the_host_buffer and copy of the_buffer
			for (j = y1; j <= y2; j++) {
				Screen_blit(the_host_buffer + i, the_buffer + i, VideoMonitor.x >> 3);
				i += bytes_per_row;
			}

		} else {

			// Update the_host_buffer and copy of the_buffer
			for (j = y1; j <= y2; j++) {
				Screen_blit(the_host_buffer + i, the_buffer + i, bytes_per_pixel * VideoMonitor.x);
				i += bytes_per_row;
			}
		}

		if (have_shm)
			XShmPutImage(x_display, the_win, the_gc, img, 0, y1, 0, y1, VideoMonitor.x, height, 0);
		else
			XPutImage(x_display, the_win, the_gc, img, 0, y1, 0, y1, VideoMonitor.x, height);
	}

	mainBuffer.dirty = false;
}


/*
 *	Update display for DGA mode and VOSF
 *	(only in Direct Addressing mode)
 */

#if REAL_ADDRESSING || DIRECT_ADDRESSING
static inline void update_display_dga_vosf(void)
{
	int page = 0;
	for (;;) {
		const int first_page = find_next_page_set(page);
		if (first_page >= mainBuffer.pageCount)
			break;

		page = find_next_page_clear(first_page);
		PFLAG_CLEAR_RANGE(first_page, page);

		// Make the dirty pages read-only again
		const int32 offset  = first_page << mainBuffer.pageBits;
		const uint32 length = (page - first_page) << mainBuffer.pageBits;
		vm_protect((char *)mainBuffer.memStart + offset, length, VM_PAGE_READ);
		
		// I am sure that y2 >= y1 and depth != 1
		const int y1 = mainBuffer.pageInfo[first_page].top;
		const int y2 = mainBuffer.pageInfo[page - 1].bottom;
		
		const int bytes_per_row = VideoMonitor.bytes_per_row;
		const int bytes_per_pixel = VideoMonitor.bytes_per_row / VideoMonitor.x;
		int i, j;
		
		// Check for first column from left and first column
		// from right that have changed
		int x1 = VideoMonitor.x * bytes_per_pixel - 1;
		for (j = y1; j <= y2; j++) {
			uint8 * const p1 = &the_buffer[j * bytes_per_row];
			uint8 * const p2 = &the_buffer_copy[j * bytes_per_row];
			for (i = 0; i < x1; i++) {
				if (p1[i] != p2[i]) {
					x1 = i;
					break;
				}
			}
		}
		x1 /= bytes_per_pixel;
		
		int x2 = x1 * bytes_per_pixel;
		for (j = y2; j >= y1; j--) {
			uint8 * const p1 = &the_buffer[j * bytes_per_row];
			uint8 * const p2 = &the_buffer_copy[j * bytes_per_row];
			for (i = VideoMonitor.x * bytes_per_pixel - 1; i > x2; i--) {
				if (p1[i] != p2[i]) {
					x2 = i;
					break;
				}
			}
		}
		x2 /= bytes_per_pixel;
		
		// Update the_host_buffer and copy of the_buffer
		// There should be at least one pixel to copy
		const int width = x2 - x1 + 1;
		i = y1 * bytes_per_row + x1 * bytes_per_pixel;
		for (j = y1; j <= y2; j++) {
			Screen_blit(the_host_buffer + i, the_buffer + i, bytes_per_pixel * width);
			memcpy(the_buffer_copy + i, the_buffer + i, bytes_per_pixel * width);
			i += bytes_per_row;
		}
	}
	mainBuffer.dirty = false;
}
#endif

#endif /* ENABLE_VOSF */

#endif /* VIDEO_VOSF_H */
