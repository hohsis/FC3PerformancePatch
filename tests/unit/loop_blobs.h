/* Hand-assembled x86 replicas of the engine's processor-enumeration loop.
 *
 * Both functions have the signature
 *
 *     unsigned __cdecl enum_loop(unsigned system_mask, unsigned (__stdcall *probe)(unsigned mask));
 *
 * and implement
 *
 *     i = 0; mask = 1;
 *     while ([mask != 0 &&] mask <= system_mask) { probe(mask); ++i; mask = 1 << i; }
 *     return i;
 *
 * with exactly the instruction sequence (and therefore the same bug) that the
 * game uses for the loop tail. They contain no engine code beyond those few
 * instructions and are position independent.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_LOOP_BLOBS_H
#define FC3PP_LOOP_BLOBS_H

/* Optimised ("release") form, as in FC3.dll / FC3_d3d11.dll. */
#define LOOP_BLOB_RELEASE                                                                              \
    0x55,                                     /* 00 push ebp                    */                     \
    0x8B, 0xEC,                               /* 01 mov  ebp, esp               */                     \
    0x83, 0xEC, 0x20,                         /* 03 sub  esp, 20h               */                     \
    0x53,                                     /* 06 push ebx                    */                     \
    0x56,                                     /* 07 push esi                    */                     \
    0x57,                                     /* 08 push edi                    */                     \
    0xC7, 0x45, 0xF0, 0x00, 0x00, 0x00, 0x00, /* 09 mov  dword [ebp-10h], 0     */                     \
    0xBE, 0x01, 0x00, 0x00, 0x00,             /* 10 mov  esi, 1                 */                     \
    0x3B, 0x75, 0x08,                         /* 15 cmp  esi, [ebp+8]     <head */                     \
    0x0F, 0x87, 0x1A, 0x00, 0x00, 0x00,       /* 18 ja   done                   */                     \
    0x56,                                     /* 1E push esi                    */                     \
    0xFF, 0x55, 0x0C,                         /* 1F call [ebp+0Ch]              */                     \
    0x8B, 0x4D, 0xF0,                         /* 22 mov  ecx, [ebp-10h]         */                     \
    0x41,                                     /* 25 inc  ecx              <site */                     \
    0xBE, 0x01, 0x00, 0x00, 0x00,             /* 26 mov  esi, 1                 */                     \
    0xD3, 0xE6,                               /* 2B shl  esi, cl                */                     \
    0x89, 0x4D, 0xF0,                         /* 2D mov  [ebp-10h], ecx         */                     \
    0x85, 0xF6,                               /* 30 test esi, esi               */                     \
    0x0F, 0x85, 0xDD, 0xFF, 0xFF, 0xFF,       /* 32 jnz  head                   */                     \
    0x8B, 0x45, 0xF0,                         /* 38 mov  eax, [ebp-10h]   <done */                     \
    0x5F,                                     /* 3B pop  edi                    */                     \
    0x5E,                                     /* 3C pop  esi                    */                     \
    0x5B,                                     /* 3D pop  ebx                    */                     \
    0x8B, 0xE5,                               /* 3E mov  esp, ebp               */                     \
    0x5D,                                     /* 40 pop  ebp                    */                     \
    0xC3                                      /* 41 ret                         */

#define LOOP_BLOB_RELEASE_SIZE 0x42
#define LOOP_BLOB_RELEASE_SITE 0x25

/* Unoptimised ("debug") form, as in systemdetection.dll. */
#define LOOP_BLOB_DEBUG                                                                                \
    0x55,                                     /* 00 push ebp                    */                     \
    0x8B, 0xEC,                               /* 01 mov  ebp, esp               */                     \
    0x83, 0xEC, 0x40,                         /* 03 sub  esp, 40h               */                     \
    0xC7, 0x45, 0xC4, 0x00, 0x00, 0x00, 0x00, /* 06 mov  dword [ebp-3Ch], 0     */                     \
    0xC7, 0x45, 0xE8, 0x01, 0x00, 0x00, 0x00, /* 0D mov  dword [ebp-18h], 1     */                     \
    0x83, 0x7D, 0xE8, 0x00,                   /* 14 cmp  dword [ebp-18h], 0 <head */                   \
    0x0F, 0x84, 0x2E, 0x00, 0x00, 0x00,       /* 18 je   done                   */                     \
    0x8B, 0x45, 0xE8,                         /* 1E mov  eax, [ebp-18h]         */                     \
    0x3B, 0x45, 0x08,                         /* 21 cmp  eax, [ebp+8]           */                     \
    0x0F, 0x87, 0x22, 0x00, 0x00, 0x00,       /* 24 ja   done                   */                     \
    0x8B, 0x45, 0xE8,                         /* 2A mov  eax, [ebp-18h]         */                     \
    0x50,                                     /* 2D push eax                    */                     \
    0xFF, 0x55, 0x0C,                         /* 2E call [ebp+0Ch]              */                     \
    0x8B, 0x45, 0xC4,                         /* 31 mov  eax, [ebp-3Ch]   <site */                     \
    0x83, 0xC0, 0x01,                         /* 34 add  eax, 1                 */                     \
    0x89, 0x45, 0xC4,                         /* 37 mov  [ebp-3Ch], eax         */                     \
    0xB8, 0x01, 0x00, 0x00, 0x00,             /* 3A mov  eax, 1                 */                     \
    0x8B, 0x4D, 0xC4,                         /* 3F mov  ecx, [ebp-3Ch]         */                     \
    0xD3, 0xE0,                               /* 42 shl  eax, cl                */                     \
    0x89, 0x45, 0xE8,                         /* 44 mov  [ebp-18h], eax         */                     \
    0xE9, 0xC8, 0xFF, 0xFF, 0xFF,             /* 47 jmp  head                   */                     \
    0x8B, 0x45, 0xC4,                         /* 4C mov  eax, [ebp-3Ch]   <done */                     \
    0x8B, 0xE5,                               /* 4F mov  esp, ebp               */                     \
    0x5D,                                     /* 51 pop  ebp                    */                     \
    0xC3                                      /* 52 ret                         */

#define LOOP_BLOB_DEBUG_SIZE 0x53
#define LOOP_BLOB_DEBUG_SITE 0x31

/* Optimised form with 32-bit stack displacements, as in Blood Dragon's
 * systemdetection.dll. */
#define LOOP_BLOB_DISP32                                                                               \
    0x55,                                     /* 00 push ebp                    */                     \
    0x8B, 0xEC,                               /* 01 mov  ebp, esp               */                     \
    0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,       /* 03 sub  esp, 200h              */                     \
    0x53,                                     /* 09 push ebx                    */                     \
    0x56,                                     /* 0A push esi                    */                     \
    0x57,                                     /* 0B push edi                    */                     \
    0xC7, 0x85, 0x34, 0xFE, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, /* 0C mov dword [ebp-1CCh], 0 */       \
    0xBE, 0x01, 0x00, 0x00, 0x00,             /* 16 mov  esi, 1                 */                     \
    0x3B, 0xB5, 0x08, 0x00, 0x00, 0x00,       /* 1B cmp  esi, [ebp+8]     <head */                     \
    0x0F, 0x87, 0x20, 0x00, 0x00, 0x00,       /* 21 ja   done                   */                     \
    0x56,                                     /* 27 push esi                    */                     \
    0xFF, 0x55, 0x0C,                         /* 28 call [ebp+0Ch]              */                     \
    0x8B, 0x8D, 0x34, 0xFE, 0xFF, 0xFF,       /* 2B mov  ecx, [ebp-1CCh]        */                     \
    0x41,                                     /* 31 inc  ecx              <site */                     \
    0xBE, 0x01, 0x00, 0x00, 0x00,             /* 32 mov  esi, 1                 */                     \
    0xD3, 0xE6,                               /* 37 shl  esi, cl                */                     \
    0x89, 0x8D, 0x34, 0xFE, 0xFF, 0xFF,       /* 39 mov  [ebp-1CCh], ecx        */                     \
    0x85, 0xF6,                               /* 3F test esi, esi               */                     \
    0x0F, 0x85, 0xD4, 0xFF, 0xFF, 0xFF,       /* 41 jnz  head                   */                     \
    0x8B, 0x85, 0x34, 0xFE, 0xFF, 0xFF,       /* 47 mov  eax, [ebp-1CCh]  <done */                     \
    0x5F,                                     /* 4D pop  edi                    */                     \
    0x5E,                                     /* 4E pop  esi                    */                     \
    0x5B,                                     /* 4F pop  ebx                    */                     \
    0x8B, 0xE5,                               /* 50 mov  esp, ebp               */                     \
    0x5D,                                     /* 52 pop  ebp                    */                     \
    0xC3                                      /* 53 ret                         */

#define LOOP_BLOB_DISP32_SIZE 0x54
#define LOOP_BLOB_DISP32_SITE 0x31

/* Size-optimised form that keeps the constant 1 in ebx, as in Blood Dragon's
 * FC3.dll / FC3_d3d11.dll. */
#define LOOP_BLOB_EBX                                                                                  \
    0x55,                                     /* 00 push ebp                    */                     \
    0x8B, 0xEC,                               /* 01 mov  ebp, esp               */                     \
    0x83, 0xEC, 0x20,                         /* 03 sub  esp, 20h               */                     \
    0x53,                                     /* 06 push ebx                    */                     \
    0x56,                                     /* 07 push esi                    */                     \
    0x57,                                     /* 08 push edi                    */                     \
    0x33, 0xDB,                               /* 09 xor  ebx, ebx               */                     \
    0x43,                                     /* 0B inc  ebx                    */                     \
    0x83, 0x65, 0xF8, 0x00,                   /* 0C and  dword [ebp-8], 0       */                     \
    0x8B, 0xF3,                               /* 10 mov  esi, ebx               */                     \
    0x3B, 0x75, 0x08,                         /* 12 cmp  esi, [ebp+8]     <head */                     \
    0x0F, 0x87, 0x16, 0x00, 0x00, 0x00,       /* 15 ja   done                   */                     \
    0x56,                                     /* 1B push esi                    */                     \
    0xFF, 0x55, 0x0C,                         /* 1C call [ebp+0Ch]              */                     \
    0xFF, 0x45, 0xF8,                         /* 1F inc  dword [ebp-8]    <site */                     \
    0x8B, 0x4D, 0xF8,                         /* 22 mov  ecx, [ebp-8]           */                     \
    0x8B, 0xF3,                               /* 25 mov  esi, ebx               */                     \
    0xD3, 0xE6,                               /* 27 shl  esi, cl                */                     \
    0x85, 0xF6,                               /* 29 test esi, esi               */                     \
    0x0F, 0x85, 0xE1, 0xFF, 0xFF, 0xFF,       /* 2B jnz  head                   */                     \
    0x8B, 0x45, 0xF8,                         /* 31 mov  eax, [ebp-8]     <done */                     \
    0x5F,                                     /* 34 pop  edi                    */                     \
    0x5E,                                     /* 35 pop  esi                    */                     \
    0x5B,                                     /* 36 pop  ebx                    */                     \
    0x8B, 0xE5,                               /* 37 mov  esp, ebp               */                     \
    0x5D,                                     /* 39 pop  ebp                    */                     \
    0xC3                                      /* 3A ret                         */

#define LOOP_BLOB_EBX_SIZE 0x3B
#define LOOP_BLOB_EBX_SITE 0x1F

#endif /* FC3PP_LOOP_BLOBS_H */
