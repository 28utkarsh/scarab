/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : pref_bingo.c
 * Author       : HPS Research Group
 * Date         : 10/24/2002
 * Description  :
 ***************************************************************************************/

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"

#include "globals/assert.h"
#include "globals/utils.h"
#include "op.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "general.param.h"
#include "libs/cache_lib.h"
#include "libs/hash_lib.h"
#include "libs/list_lib.h"
#include "memory/memory.param.h"
#include "prefetcher//pref_bingo.h"
#include "prefetcher//pref_bingo.param.h"
#include "prefetcher/pref.param.h"
#include "prefetcher/pref_common.h"
#include "statistics.h"

/**************************************************************************************/
/* Macros */
#define DEBUG(args...) _DEBUG(DEBUG_PREF_BINGO, ##args)

/**************************************************************************************/
Hash_Table History_Table;
Hash_Table Aux_Storage;
HWP_Info   hwp_in;

/* Helper: page number and indices */

static inline Addr bingo_get_page_number(Addr addr) {
  // 4KB pages → remove low 12 bits
  return addr >> 12;
}

static inline Addr bingo_get_page_base_from_number(Addr page_num) {
  return page_num << 12;
}

static inline int bingo_get_block_index(Addr addr) {
  // Page offset (lower 12 bits), then divide by 64B line size
  Addr page_offset = addr & (4096 - 1);
  return (int)(page_offset >> 6); // 64-byte blocks
}

/**************************************************************************************/

void pref_bingo_init(HWP* hwp) {
  if(!PREF_BINGO_ON)
    return;

  hwp->hwp_info->enabled = TRUE;
  hwp_in                 = *(hwp->hwp_info);

  init_hash_table(&History_Table, "History Table", 2048,
                  sizeof(Bingo_Table_Line));
  init_hash_table(&Aux_Storage, "Auxiliary Storage", 2048, sizeof(Aux_Entry));

  DEBUG("Bingo prefetcher initialized\n");
}

/**************************************************************************************/

void pref_bingo_ul1_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                        uns32 global_hist) {
  // On hit, we only update the footprint bitmap (no prefetching).

  Addr page_number = bingo_get_page_number(lineAddr);
  int  block_index = bingo_get_block_index(lineAddr);

  Aux_Entry* aux_entry = (Aux_Entry*)hash_table_access(&Aux_Storage,
                                                       page_number);

  if(aux_entry) {
    aux_entry->footprint.accessed[block_index] = TRUE;
  } else {
    Aux_Entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.trigger_addr = lineAddr;
    new_entry.pc           = loadPC;
    new_entry.footprint.accessed[block_index] = TRUE;

    // Store the new auxiliary entry in the aux table (copied into hash storage)
    hash_table_access_replace(&Aux_Storage, page_number, &new_entry);
  }

  (void)proc_id;
  (void)global_hist;
}

/**************************************************************************************/

void pref_bingo_ul1_cache_evict(uns8 proc_id, Addr lineAddr) {
  (void)proc_id;

  Addr page_number = bingo_get_page_number(lineAddr);

  // Access the auxiliary entry from the Aux_Storage table
  Aux_Entry* aux_entry =
    (Aux_Entry*)hash_table_access(&Aux_Storage, page_number);

  if(aux_entry == NULL) {
    // No aux entry for this page; nothing to promote to history
    return;
  }

  // Use line index (global) as "offset" component for PC + offset
  Addr block_address  = lineAddr >> 6; // line index
  Addr pc_plus_offset = aux_entry->pc + block_address;
  Addr pc_plus_address = aux_entry->pc + lineAddr;

  // Build a history table entry on the stack
  Bingo_History_Table hist_entry;
  hist_entry.pc_plus_address = pc_plus_address;
  hist_entry.pc_plus_offset  = pc_plus_offset;
  hist_entry.entry           = *aux_entry; // copy aux data

  // Access history line by pc_plus_offset; allocate new line if needed
  Bingo_Table_Line* table_line =
    (Bingo_Table_Line*)hash_table_access(&History_Table, pc_plus_offset);

  Bingo_Table_Line new_line;
  if(table_line == NULL) {
    memset(&new_line, 0, sizeof(new_line));
    table_line = &new_line;
  }

  add_entry(table_line, hist_entry);

  // Replace / insert line in history table
  hash_table_access_replace(&History_Table, pc_plus_offset, table_line);

  // Remove from auxiliary storage; page is no longer tracked there
  hash_table_access_delete(&Aux_Storage, page_number);
}

/**************************************************************************************/

void pref_bingo_ul1_miss(uns8 proc_id, Addr lineAddr, Addr loadPC,
                         uns32 global_hist) {
  // On miss:
  // 1) Try exact PC+Address in history.
  // 2) If not found, try the most recent entry with same PC+Offset.
  // 3) If still not found, update or create the Aux_Storage footprint.

  Addr block_address = (lineAddr >> 6) << 6; // aligned line address
  Addr pc_plus_offset = loadPC + block_address;
  Addr pc_plus_address = loadPC + lineAddr;
  Addr page_number = bingo_get_page_number(lineAddr);
  int  block_index = bingo_get_block_index(lineAddr);

  Bingo_Table_Line* line =
    (Bingo_Table_Line*)hash_table_access(&History_Table, pc_plus_offset);

  if(line == NULL) {
    // No history yet: just update auxiliary footprint for this page
    Aux_Entry* aux_entry =
      (Aux_Entry*)hash_table_access(&Aux_Storage, page_number);

    if(aux_entry) {
      aux_entry->footprint.accessed[block_index] = TRUE;
    } else {
      Aux_Entry new_entry;
      memset(&new_entry, 0, sizeof(new_entry));
      new_entry.trigger_addr = lineAddr;
      new_entry.pc           = loadPC;
      new_entry.footprint.accessed[block_index] = TRUE;

      hash_table_access_replace(&Aux_Storage, page_number, &new_entry);
    }
    return;
  }

  // We have a history line: first try exact PC+Address
  Bingo_History_Table* hash_entry =
    pref_bingo_find_event_to_fetch_addr(line, pc_plus_address);

  if(hash_entry == NULL) {
    // Fall back to most recent with same PC+Offset
    hash_entry = pref_bingo_find_event_to_fetch(line, pc_plus_offset);
  }

  Aux_Entry* aux_entry =
    (Aux_Entry*)hash_table_access(&Aux_Storage, page_number);

  if(hash_entry) {
    // Use the history entry to prefetch
    pref_bingo_prefetch(*hash_entry, proc_id, page_number);
    mark_used_by_address(line, pc_plus_address);
    return;
  }

  // No history event chosen → keep learning via Aux_Storage
  if(aux_entry) {
    aux_entry->footprint.accessed[block_index] = TRUE;
  } else {
    Aux_Entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.trigger_addr = lineAddr;
    new_entry.pc           = loadPC;
    new_entry.footprint.accessed[block_index] = TRUE;

    hash_table_access_replace(&Aux_Storage, page_number, &new_entry);
  }

  (void)global_hist;
}

/**************************************************************************************/
/* History-table search helpers */

/* Finds the most recently used entry with a matching pc_plus_offset */
Bingo_History_Table*
pref_bingo_find_event_to_fetch(Bingo_Table_Line* table_line,
                               Addr              pc_plus_offset) {
  if(History_Table.count == 0) {
    return NULL;
  }
  for(int i = 0; i < table_line->current_size; i++) {
    int index = table_line->usage_order[i];
    if(table_line->line[index].pc_plus_offset == pc_plus_offset) {
      return &table_line->line[index];
    }
  }
  return NULL; // Not found
}

/* Finds the most recently used entry with a matching pc_plus_address */
Bingo_History_Table*
pref_bingo_find_event_to_fetch_addr(Bingo_Table_Line* table_line,
                                    Addr              pc_plus_address) {
  if(History_Table.count == 0) {
    return NULL;
  }

  for(int i = 0; i < table_line->current_size; i++) {
    int index = table_line->usage_order[i];
    if(table_line->line[index].pc_plus_address == pc_plus_address) {
      return &table_line->line[index];
    }
  }
  return NULL; // Not found
}

/**************************************************************************************/

/*
 * Prefetch all cache lines whose footprint bits are set.
 * Here, page_address is interpreted as a page *number* (lineAddr >> 12).
 */
void pref_bingo_prefetch(Bingo_History_Table History_Entry,
                         uns8               proc_id,
                         Addr               page_address) {
  Addr page_base = bingo_get_page_base_from_number(page_address);

  for(int i = 0; i < 64; i++) {
    if(History_Entry.entry.footprint.accessed[i] == TRUE) {
      // Convert page_base + i*64 bytes into a line index for the prefetch API
      Addr line_addr  = page_base + ((Addr)i << 6); // i * 64
      uns  line_index = (uns)(line_addr >> LOG2(DCACHE_LINE_SIZE));

      pref_addto_ul1req_queue(proc_id, line_index, hwp_in.id);
    }
  }
}

/**************************************************************************************/

/*
 * Adds a new entry to the table, treats it as the most recently used,
 * and evicts the least recently used if the line is full.
 */
void add_entry(Bingo_Table_Line* table_line, Bingo_History_Table new_entry) {
  int index_to_replace;
  int used = table_line->current_size;

  if(used < 16) {
    // If there's space, add to the next available spot
    index_to_replace = used;
    table_line->current_size++;
    used++; // number of valid entries after insertion
  } else {
    // Replace the least recently used (last in usage_order array)
    index_to_replace = table_line->usage_order[15];
    used             = 16;
  }

  // Replace the entry at the chosen index
  table_line->line[index_to_replace] = new_entry;

  // Shift the usage_order to the right in the valid range
  for(int i = used - 1; i > 0; i--) {
    table_line->usage_order[i] = table_line->usage_order[i - 1];
  }

  // Place the new entry's index at the front (most recently used)
  table_line->usage_order[0] = index_to_replace;
}

/*
 * Marks an entry as recently used by its pc_plus_address.
 */
void mark_used_by_address(Bingo_Table_Line* table_line, Addr pc_plus_address) {
  int found_index = -1;

  // Find the entry index by address
  for(int i = 0; i < table_line->current_size; i++) {
    if(table_line->line[i].pc_plus_address == pc_plus_address) {
      found_index = i;
      break;
    }
  }

  if(found_index == -1)
    return; // Entry not found

  // Move the found entry to the front of usage_order (MRU)
  for(int i = 0; i < table_line->current_size; i++) {
    if(table_line->usage_order[i] == found_index) {
      // Shift entries to the right up to position i
      for(int j = i; j > 0; j--) {
        table_line->usage_order[j] = table_line->usage_order[j - 1];
      }
      table_line->usage_order[0] = found_index;
      break;
    }
  }
}
