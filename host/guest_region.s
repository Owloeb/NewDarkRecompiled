/* Reserves the game's address range [0x401000, 0xD25000) inside the host image so the OS loader claims it at process
 * creation, before any heap/stack/NLS mapping can. Linked FIRST with --image-base=0x400000, so it is the first
 * contribution to .text and sits at 0x401000. (~9 MB of zeros in the file; the host overwrites it with the game image.) */
    .text
    .globl _guest_region
_guest_region:
    .skip 0x924000
