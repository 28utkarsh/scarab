#ifndef __PREF_BINGO_H__
#define __PREF_BINGO_H__

#include "pref_common.h"
#include <stdbool.h>

typedef struct Footprint_struct {
    bool accessed[64];  // Size of page divided by block size: 4096 / 64
} Footprint;

typedef struct Aux_Entry_Struct {
  Footprint footprint;   // Bit vector for accessed blocks within a page
  Addr      trigger_addr; // The address that triggered this entry
  Addr      pc;           // The PC of the trigger instruction
} Aux_Entry;

// This will be put into a dictionary with key as Page address
typedef struct Bingo_History_Table_Struct {
  // table hashed by PC + Offset and each entry is PC+Address, footprint
  Addr      pc_plus_address;
  Addr      pc_plus_offset;
  Aux_Entry entry;        // Holds the original data from the Aux data
} Bingo_History_Table;    // one entry in the Bingo table (also in a dictionary)

typedef struct Bingo_Table_Line_Struct {
    Bingo_History_Table line[16];  // The entries
    int                 usage_order[16]; // Tracks order of usage for LRU
    int                 current_size;    // Number of valid entries in the line
} Bingo_Table_Line;

void pref_bingo_init(HWP* hwp);

void pref_bingo_ul1_cache_evict(uns8 proc_id, Addr lineAddr);

void pref_bingo_ul1_miss(uns8 proc_id, Addr lineAddr, Addr loadPC,
                         uns32 global_hist);
void pref_bingo_ul1_hit(uns8 proc_id, Addr lineAddr, Addr loadPC,
                        uns32 global_hist);

Bingo_History_Table*
pref_bingo_find_event_to_fetch(Bingo_Table_Line* table_line,
                               Addr pc_plus_offset);

Bingo_History_Table*
pref_bingo_find_event_to_fetch_addr(Bingo_Table_Line* table_line,
                                    Addr pc_plus_address);

void pref_bingo_prefetch(Bingo_History_Table History_Entry,
                         uns8               proc_id,
                         Addr               page_address);

void add_entry(Bingo_Table_Line* table_line, Bingo_History_Table new_entry);

void mark_used_by_address(Bingo_Table_Line* table_line, Addr pc_plus_address);

#endif
