/*
 *  ReactOS kernel
 *  Copyright (C) 2000 David Welch <welch@cwcom.net>
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
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * FILE:            ntoskrnl/ke/i386/vm86_sup.S
 * PURPOSE:         V86 mode support
 * PROGRAMMER:      David Welch (welch@cwcom.net)
 * UPDATE HISTORY:
 *                  Created 10/12/00
 */

#ifndef __NTOSKRNL_INCLUDE_INTERNAL_VM86_H
#define __NTOSKRNL_INCLUDE_INTERNAL_VM86_H

#ifndef ASSEMBLER

typedef struct
{
   ULONG Ebp;
   ULONG Edi;
   ULONG Esi;
   ULONG Edx;
   ULONG Ecx;
   ULONG Ebx;
   ULONG Eax;
   ULONG Ds;
   ULONG Es;
   ULONG Fs;
   ULONG Gs;
   ULONG Eip;
   ULONG Cs;
   ULONG Eflags;
   ULONG Esp;
   ULONG Ss;
} KV86M_REGISTERS;

#else

/*
 * Definitions for the offsets of members in the KV86M_REGISTERS
 */
#define	KV86M_REGISTERS_EBP	(0x0)
#define	KV86M_REGISTERS_EDI	(0x4)
#define	KV86M_REGISTERS_ESI	(0x8)
#define KV86M_REGISTERS_EDX	(0xC)
#define	KV86M_REGISTERS_ECX	(0x10)
#define KV86M_REGISTERS_EBX	(0x14)
#define KV86M_REGISTERS_EAX	(0x18)
#define	KV86M_REGISTERS_DS	(0x1C)
#define KV86M_REGISTERS_ES	(0x20)
#define KV86M_REGISTERS_FS	(0x24)
#define KV86M_REGISTERS_GS	(0x28)
#define KV86M_REGISTERS_EIP     (0x2C)
#define KV86M_REGISTERS_CS      (0x30)
#define KV86M_REGISTERS_EFLAGS  (0x34)
#define	KV86M_REGISTERS_ESP     (0x38)
#define KV86M_REGISTERS_SS	(0x3C)

#endif /* ASSEMBLER */

#endif /* __NTOSKRNL_INCLUDE_INTERNAL_VM86_H */
