/* Hand-assembled x86 stand-ins for the two texture methods the texture guard
 * wraps. Each starts with the instruction sequence the guard recognises (the
 * method asks the Direct3D texture at [this+1Ch] for its description) and
 * then does something a test can observe instead of the engine's work.
 *
 * The object they operate on:
 *
 *     +00h  virtual table
 *     +04h  number of calls that reached "add top level"
 *     +08h  number of calls that reached "drop top level"
 *     +0Ch  number of destructor calls        (maintained by the fixture)
 *     +10h  size argument of the last add
 *     +14h  non-zero: the next add fails, like the engine running out of memory
 *     +1Ch  fake Direct3D texture: { table, width, height, mip levels, format }
 *
 * They contain no engine code beyond the recognised prologues and are
 * position independent.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEX_BLOBS_H
#define FC3PP_TEX_BLOBS_H

/* bool __thiscall AddTopLevel(const void *dds, unsigned size, void *buffer, bool flag)
 * as the optimised ("release") build of FC3_d3d11.dll begins it. */
#define TEX_BLOB_ADD_RELEASE                                                                                \
    0x55,                                           /* 00 push ebp                         */               \
    0x8B, 0xEC,                                     /* 01 mov  ebp, esp                    */               \
    0x83, 0xE4, 0xF0,                               /* 03 and  esp, -16                    */               \
    0x81, 0xEC, 0x34, 0x02, 0x00, 0x00,             /* 06 sub  esp, 234h                   */               \
    0x53,                                           /* 0C push ebx                         */               \
    0x56,                                           /* 0D push esi                         */               \
    0x8B, 0xD9,                                     /* 0E mov  ebx, ecx          ; this    */               \
    0x8B, 0x43, 0x1C,                               /* 10 mov  eax, [ebx+1Ch]    ; texture */               \
    0x8B, 0x08,                                     /* 13 mov  ecx, [eax]                  */               \
    0x57,                                           /* 15 push edi                         */               \
    0x8D, 0x94, 0x24, 0xA0, 0x00, 0x00, 0x00,       /* 16 lea  edx, [esp+0A0h]             */               \
    0x52,                                           /* 1D push edx                         */               \
    0x50,                                           /* 1E push eax                         */               \
    0x8B, 0x41, 0x28,                               /* 1F mov  eax, [ecx+28h]              */               \
    0xFF, 0xD0,                                     /* 22 call eax               ; GetDesc */               \
    0x8B, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00,       /* 24 mov  ecx, [esp+0A0h]             */               \
    0x66, 0x0F, 0x6F, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, /* 2B movdqa xmm0, [esp+0A0h]    */               \
    /* ---- end of the recognised part ---- */                                                              \
    0xFF, 0x43, 0x04,                               /* 34 inc  dword [ebx+4]     ; calls   */               \
    0x80, 0x7B, 0x14, 0x00,                         /* 37 cmp  byte [ebx+14h], 0           */               \
    0x74, 0x04,                                     /* 3B je   ok                          */               \
    0x32, 0xC0,                                     /* 3D xor  al, al            ; failed  */               \
    0xEB, 0x14,                                     /* 3F jmp  out                         */               \
    0x8B, 0x45, 0x0C,                               /* 41 mov  eax, [ebp+0Ch]    <ok: size */               \
    0x89, 0x43, 0x10,                               /* 44 mov  [ebx+10h], eax              */               \
    0x8B, 0x43, 0x1C,                               /* 47 mov  eax, [ebx+1Ch]              */               \
    0xD1, 0x60, 0x04,                               /* 4A shl  dword [eax+4], 1  ; width   */               \
    0xD1, 0x60, 0x08,                               /* 4D shl  dword [eax+8], 1  ; height  */               \
    0xFF, 0x40, 0x0C,                               /* 50 inc  dword [eax+0Ch]   ; levels  */               \
    0xB0, 0x01,                                     /* 53 mov  al, 1                       */               \
    0x5F,                                           /* 55 pop  edi               <out      */               \
    0x5E,                                           /* 56 pop  esi                         */               \
    0x5B,                                           /* 57 pop  ebx                         */               \
    0x8B, 0xE5,                                     /* 58 mov  esp, ebp                    */               \
    0x5D,                                           /* 5A pop  ebp                         */               \
    0xC2, 0x10, 0x00                                /* 5B ret  10h                         */

#define TEX_BLOB_ADD_RELEASE_SIZE 0x5E

/* bool __thiscall DropTopLevel(void), same build. Returns false without doing
 * anything when the texture has a single level, like the original. */
#define TEX_BLOB_DROP_RELEASE                                                                               \
    0x55,                                           /* 00 push ebp                         */               \
    0x8B, 0xEC,                                     /* 01 mov  ebp, esp                    */               \
    0x83, 0xEC, 0x5C,                               /* 03 sub  esp, 5Ch                    */               \
    0x53,                                           /* 06 push ebx                         */               \
    0x57,                                           /* 07 push edi                         */               \
    0x8B, 0xF9,                                     /* 08 mov  edi, ecx          ; this    */               \
    0x8B, 0x47, 0x1C,                               /* 0A mov  eax, [edi+1Ch]    ; texture */               \
    0x8B, 0x08,                                     /* 0D mov  ecx, [eax]                  */               \
    0x8D, 0x55, 0xA4,                               /* 0F lea  edx, [ebp-5Ch]              */               \
    0x52,                                           /* 12 push edx                         */               \
    0x50,                                           /* 13 push eax                         */               \
    0x8B, 0x41, 0x28,                               /* 14 mov  eax, [ecx+28h]              */               \
    0xFF, 0xD0,                                     /* 17 call eax               ; GetDesc */               \
    0xBB, 0x01, 0x00, 0x00, 0x00,                   /* 19 mov  ebx, 1                      */               \
    0x39, 0x5D, 0xAC,                               /* 1E cmp  [ebp-54h], ebx    ; levels  */               \
    0x75, 0x08,                                     /* 21 jne  more                        */               \
    0x5F,                                           /* 23 pop  edi                         */               \
    0x32, 0xC0,                                     /* 24 xor  al, al                      */               \
    0x5B,                                           /* 26 pop  ebx                         */               \
    0x8B, 0xE5,                                     /* 27 mov  esp, ebp                    */               \
    0x5D,                                           /* 29 pop  ebp                         */               \
    0xC3,                                           /* 2A ret                              */               \
    0x8B, 0x45, 0xA4,                               /* 2B mov  eax, [ebp-5Ch]    <more     */               \
    0x83, 0xF8, 0x08,                               /* 2E cmp  eax, 8                      */               \
    /* ---- end of the recognised part ---- */                                                              \
    0xFF, 0x47, 0x08,                               /* 31 inc  dword [edi+8]     ; calls   */               \
    0x8B, 0x47, 0x1C,                               /* 34 mov  eax, [edi+1Ch]              */               \
    0xD1, 0x68, 0x04,                               /* 37 shr  dword [eax+4], 1  ; width   */               \
    0xD1, 0x68, 0x08,                               /* 3A shr  dword [eax+8], 1  ; height  */               \
    0xFF, 0x48, 0x0C,                               /* 3D dec  dword [eax+0Ch]   ; levels  */               \
    0xB0, 0x01,                                     /* 40 mov  al, 1                       */               \
    0x5F,                                           /* 42 pop  edi                         */               \
    0x5B,                                           /* 43 pop  ebx                         */               \
    0x8B, 0xE5,                                     /* 44 mov  esp, ebp                    */               \
    0x5D,                                           /* 46 pop  ebp                         */               \
    0xC3                                            /* 47 ret                              */

#define TEX_BLOB_DROP_RELEASE_SIZE 0x48

/* void *__thiscall Destroy(unsigned flags): the scalar deleting destructor the
 * class's table starts with. The "destructor" it calls counts at [this+0Ch]. */
#define TEX_BLOB_DESTROY                                                                                    \
    0x55,                                           /* 00 push ebp                         */               \
    0x8B, 0xEC,                                     /* 01 mov  ebp, esp                    */               \
    0x56,                                           /* 03 push esi                         */               \
    0x8B, 0xF1,                                     /* 04 mov  esi, ecx          ; this    */               \
    0xE8, 0x0D, 0x00, 0x00, 0x00,                   /* 06 call destructor                  */               \
    0xF6, 0x45, 0x08, 0x01,                         /* 0B test byte [ebp+8], 1             */               \
    0x74, 0x00,                                     /* 0F jz   +0     ; nothing to free    */               \
    /* ---- end of the recognised part ---- */                                                              \
    0x8B, 0xC6,                                     /* 11 mov  eax, esi                    */               \
    0x5E,                                           /* 13 pop  esi                         */               \
    0x5D,                                           /* 14 pop  ebp                         */               \
    0xC2, 0x04, 0x00,                               /* 15 ret  4                           */               \
    0xFF, 0x41, 0x0C,                               /* 18 inc  dword [ecx+0Ch]  <destructor */              \
    0xC3                                            /* 1B ret                              */

#define TEX_BLOB_DESTROY_SIZE 0x1C

/* The first bytes of the same two methods in the size-optimised build
 * (Blood Dragon). Only used to test recognition; never executed. */
#define TEX_BLOB_ADD_SIZE_HEAD                                                                              \
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x38, 0x02, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xD9, 0x8B, 0x43, 0x1C, 0x8B, 0x08, \
    0x57, 0x8D, 0x55, 0x88, 0x52, 0x50, 0x89, 0x5D, 0xFC, 0xFF, 0x51, 0x28, 0x8B, 0x45, 0x88, 0x6A, 0x0B, 0x03, \
    0xC0, 0x59, 0x8D, 0x75, 0x88, 0x8D, 0x7D, 0xB4, 0xF3, 0xA5, 0xC3

#define TEX_BLOB_DROP_SIZE_HEAD                                                                             \
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x5C, 0x53, 0x56, 0x57, 0x8B, 0xD9, 0x8B, 0x43, 0x1C, 0x8B, 0x08, 0x8D, 0x55, \
    0xA4, 0x52, 0x50, 0xFF, 0x51, 0x28, 0x33, 0xD2, 0x42, 0x39, 0x55, 0xAC, 0x0F, 0x84, 0x6B, 0x01, 0x00, 0x00, \
    0x8B, 0x45, 0xA4, 0x83, 0xF8, 0x08, 0xC3

/* ---- the "top level resource" class (tex_link.h) -----------------------------------
 * Stand-ins for its Load and Unload methods. Each starts with the instructions
 * the patch recognises and then follows the link to the texture the way the
 * real method does, so that a link to a texture that is gone crashes here as
 * it does in the game.
 *
 *   resource:  +04h  calls that reached Load     +08h  calls that reached Unload
 *              +20h  file name (64 bit)          +28h  size, cleared by Unload
 *              +30h  linked texture              +34h  valid
 *   texture:   +28h  top levels added            +30h  record     +48h  users
 *   record:    +20h  counter, decremented when a top level is dropped
 */

/* bool __thiscall Load(const void *file, unsigned size): al = 0 "added". */
#define TOP_BLOB_LOAD                                                                                       \
    0x55,                                           /* 00 push ebp                         */               \
    0x8B, 0xEC,                                     /* 01 mov  ebp, esp                    */               \
    0x83, 0xEC, 0x2C,                               /* 03 sub  esp, 2Ch                    */               \
    0x56,                                           /* 06 push esi                         */               \
    0x57,                                           /* 07 push edi                         */               \
    0x8B, 0x7D, 0x0C,                               /* 08 mov  edi, [ebp+0Ch]    ; size    */               \
    0x8B, 0xF1,                                     /* 0B mov  esi, ecx          ; this    */               \
    0x8B, 0x4D, 0x08,                               /* 0D mov  ecx, [ebp+8]      ; file    */               \
    0x57,                                           /* 10 push edi                         */               \
    0x8D, 0x45, 0xD4,                               /* 11 lea  eax, [ebp-2Ch]              */               \
    /* ---- end of the recognised part ---- */                                                              \
    0x83, 0xC4, 0x04,                               /* 14 add  esp, 4                      */               \
    0xFF, 0x46, 0x04,                               /* 17 inc  dword [esi+4]     ; calls   */               \
    0x8B, 0x46, 0x30,                               /* 1A mov  eax, [esi+30h]    ; texture */               \
    0xFF, 0x40, 0x28,                               /* 1D inc  dword [eax+28h]   ; added   */               \
    0x32, 0xC0,                                     /* 20 xor  al, al                      */               \
    0x5F,                                           /* 22 pop  edi                         */               \
    0x5E,                                           /* 23 pop  esi                         */               \
    0x8B, 0xE5,                                     /* 24 mov  esp, ebp                    */               \
    0x5D,                                           /* 26 pop  ebp                         */               \
    0xC2, 0x08, 0x00                                /* 27 ret  8                           */

#define TOP_BLOB_LOAD_SIZE 0x2A

/* void __thiscall Unload(void). */
#define TOP_BLOB_UNLOAD                                                                                     \
    0x55,                                           /* 00 push ebp                         */               \
    0x8B, 0xEC,                                     /* 01 mov  ebp, esp                    */               \
    0x51,                                           /* 03 push ecx                         */               \
    0x80, 0x79, 0x34, 0x00,                         /* 04 cmp  byte [ecx+34h], 0 ; valid?  */               \
    0x89, 0x4D, 0xFC,                               /* 08 mov  [ebp-4], ecx                */               \
    0x74, 0x24,                                     /* 0B je   out                         */               \
    0x53,                                           /* 0D push ebx                         */               \
    0x8B, 0x59, 0x30,                               /* 0E mov  ebx, [ecx+30h]    ; texture */               \
    0x83, 0x7B, 0x48, 0x01,                         /* 11 cmp  dword [ebx+48h], 1 ; users  */               \
    0x77, 0x0C,                                     /* 15 ja   done                        */               \
    0x56,                                           /* 17 push esi                         */               \
    0x8B, 0x73, 0x30,                               /* 18 mov  esi, [ebx+30h]    ; record  */               \
    0x85, 0xF6,                                     /* 1B test esi, esi                    */               \
    0x74, 0x03,                                     /* 1D je   none                        */               \
    /* ---- end of the recognised part ---- */                                                              \
    0xFF, 0x4E, 0x20,                               /* 1F dec  dword [esi+20h]   ; "Drop"  */               \
    0x5E,                                           /* 22 pop  esi               <none     */               \
    0x8B, 0x4D, 0xFC,                               /* 23 mov  ecx, [ebp-4]      <done     */               \
    0xFF, 0x41, 0x08,                               /* 26 inc  dword [ecx+8]     ; calls   */               \
    0xC7, 0x41, 0x28, 0x00, 0x00, 0x00, 0x00,       /* 29 mov  dword [ecx+28h], 0          */               \
    0x5B,                                           /* 30 pop  ebx                         */               \
    0x8B, 0xE5,                                     /* 31 mov  esp, ebp          <out      */               \
    0x5D,                                           /* 33 pop  ebp                         */               \
    0xC3                                            /* 34 ret                              */

#define TOP_BLOB_UNLOAD_SIZE 0x35

#endif /* FC3PP_TEX_BLOBS_H */
