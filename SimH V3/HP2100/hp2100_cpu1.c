/* hp2100_cpu1.c: HP 2100/1000 EAU simulator and UIG dispatcher

   Copyright (c) 2005-2007, Robert M. Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   CPU1         Extended arithmetic and optional microcode dispatchers

   04-Jan-07    JDB     Added special DBI dispatcher for non-INT64 diagnostic
   29-Dec-06    JDB     Allows RRR as NOP if 2114 (diag config test)
   01-Dec-06    JDB     Substitutes FPP for firmware FP if HAVE_INT64
   16-Oct-06    JDB     Generalized operands for F-Series FP types
   26-Sep-06    JDB     Split hp2100_cpu1.c into multiple modules to simplify extensions
                        Added iotrap parameter to UIG dispatchers for RTE microcode
   22-Feb-05    JDB     Removed EXECUTE instruction (is NOP in actual microcode)
   21-Jan-05    JDB     Reorganized CPU option and operand processing flags
                        Split code along microcode modules
   15-Jan-05    RMS     Cloned from hp2100_cpu.c

   Primary references:
   - HP 1000 M/E/F-Series Computers Technical Reference Handbook
        (5955-0282, Mar-1980)
   - HP 1000 M/E/F-Series Computers Engineering and Reference Documentation
        (92851-90001, Mar-1981)
   - Macro/1000 Reference Manual (92059-90001, Dec-1992)

   Additional references are listed with the associated firmware
   implementations, as are the HP option model numbers pertaining to the
   applicable CPUs.

   This source file contains the Extended Arithmetic Unit simulator and the User
   Instruction Group (a.k.a. "Macro") dispatcher for the 2100 and 1000 (21MX)
   CPUs.  The UIG simulators reside in separate source files, due to the large
   number of firmware options available for the 1000 machines.  Unit flags
   indicate which options are present in the current system.

   This module also provides generalized instruction operand processing.

   The microcode address space of the 2100 encompassed four modules of 256 words
   each.  The 1000 M-series expanded that to sixteen modules, and the 1000
   E/F-series expanded that still further to sixty-four modules.  Each CPU had
   its own microinstruction set, although the micromachines of the various 1000
   models were similar internally.

   Regarding option instruction sets, there was some commonality across CPU
   types.  EAU instructions were identical across all models, and the floating
   point set was the same on the 2100 and 1000.  Other options implemented
   proper instruction supersets (e.g., the Fast FORTRAN Processor from 2100 to
   1000-M to 1000-E to 1000-F) or functional equivalence with differing code
   points (the 2000 I/O Processor from 2100 to 1000 and extended-precision
   floating-point instructions from 1000-E to 1000-F).

   The 2100 decoded the EAU and UIG sets separately in hardware and supported
   only the UIG 0 code points.  Bits 7-4 of a UIG instruction decoded one of
   sixteen entry points in the lowest-numbered module after module 0.  Those
   entry points could be used directly (as for the floating-point instructions),
   or additional decoding based on bits 3-0 could be implemented.

   The 1000 generalized the instruction decoding to a series of microcoded
   jumps, based on the bits in the instruction.  Bits 15-8 indicated the group
   of the current instruction: EAU (200, 201, 202, 210, and 211), UIG 0 (212),
   or UIG 1 (203 and 213).  UIG 0, UIG 1, and some EAU instructions were decoded
   further by selecting one of sixteen modules within the group via bits 7-4.
   Finally, each UIG module decoded up to sixteen instruction entry points via
   bits 3-0.  Jump tables for all firmware options were contained in the base
   set, so modules needed only to be concerned with decoding their individual
   entry points within the module.

   While the 2100 and 1000 hardware decoded these instruction sets differently,
   the decoding mechanism of the simulation follows that of the 1000 E/F-series.
   Where needed, CPU type- or model-specific behavior is simulated.

   The design of the 1000 microinstruction set was such that executing an
   instruction for which no microcode was present (e.g., executing a FFP
   instruction when the FFP firmware was not installed) resulted in a NOP.
   Under simulation, such execution causes an undefined instruction stop.
*/

#include "hp2100_defs.h"
#include "hp2100_cpu.h"
#include "hp2100_cpu1.h"

#if defined (HAVE_INT64)                                /* int64 support available */
extern t_stat cpu_fpp (uint32 IR, uint32 intrq);        /* Floating Point Processor */
extern t_stat cpu_sis (uint32 IR, uint32 intrq);        /* Scientific Instruction */
#else                                                   /* int64 support unavailable */
extern t_stat cpu_fp  (uint32 IR, uint32 intrq);        /* Firmware Floating Point */
#endif                                                  /* end of int64 support */

extern t_stat cpu_ffp (uint32 IR, uint32 intrq);        /* Fast FORTRAN Processor */
extern t_stat cpu_ds  (uint32 IR, uint32 intrq);        /* Distributed Systems */
extern t_stat cpu_vis (uint32 IR, uint32 intrq);        /* Vector Instruction */
extern t_stat cpu_dbi (uint32 IR, uint32 intrq);        /* Double integer */
extern t_stat cpu_rte_vma (uint32 IR, uint32 intrq);    /* RTE-4/6 EMA/VMA */
extern t_stat cpu_rte_os (uint32 IR, uint32 intrq, uint32 iotrap);  /* RTE-6 OS */
extern t_stat cpu_iop (uint32 IR, uint32 intrq);        /* 2000 I/O Processor */
extern t_stat cpu_signal  (uint32 IR, uint32 intrq);    /* SIGNAL/1000 Instructions */
extern t_stat cpu_dms (uint32 IR, uint32 intrq);        /* Dynamic mapping system */
extern t_stat cpu_eig (uint32 IR, uint32 intrq);        /* Extended instruction group */

/* EAU

   The Extended Arithmetic Unit (EAU) adds ten instructions with double-word
   operands, including multiply, divide, shifts, and rotates.  Option
   implementation by CPU was as follows:

      2114    2115    2116    2100   1000-M  1000-E  1000-F
     ------  ------  ------  ------  ------  ------  ------
      N/A    12579A  12579A   std     std     std     std

   The instruction codes are mapped to routines as follows:

     Instr.    Bits
      Code   15-8 7-4   2116    2100   1000-M  1000-E  1000-F  Note
     ------  ---- ---  ------  ------  ------  ------  ------  ---------------------
     100000   200  00                          [diag]  [diag]  [self test]
     100020   200  01   ASL     ASL     ASL     ASL     ASL    Bits 3-0 encode shift
     100040   200  02   LSL     LSL     LSL     LSL     LSL    Bits 3-0 encode shift
     100060   200  03                          TIMER   TIMER   [deterministic delay]
     100100   200  04   RRL     RRL     RRL     RRL     RRL    Bits 3-0 encode shift
     100200   200  10   MPY     MPY     MPY     MPY     MPY
     100400   201  xx   DIV     DIV     DIV     DIV     DIV
     101020   202  01   ASR     ASR     ASR     ASR     ASR    Bits 3-0 encode shift
     101040   202  02   LSR     LSR     LSR     LSR     LSR    Bits 3-0 encode shift
     101100   202  04   RRR     RRR     RRR     RRR     RRR    Bits 3-0 encode shift
     104200   210  xx   DLD     DLD     DLD     DLD     DLD
     104400   211  xx   DST     DST     DST     DST     DST

   The remaining codes for bits 7-4 are undefined and will cause a simulator
   stop if enabled.  On a real 1000-M, all undefined instructions in the 200
   group decode as MPY, and all in the 202 group decode as NOP.  On a real
   1000-E, instruction patterns 200/05 through 200/07 and 202/03 decode as NOP;
   all others cause erroneous execution.

   EAU instruction decoding on the 1000 M-series is convoluted.  The JEAU
   microorder maps IR bits 11, 9-7 and 5-4 to bits 2-0 of the microcode jump
   address.  The map is detailed on page IC-84 of the ERD.

   The 1000 E/F-series add two undocumented instructions to the 200 group: TIMER
   and DIAG.  These are described in the ERD on page IA 5-5, paragraph 5-7.  The
   M-series executes these as MPY and RRL, respectively.  A third instruction,
   EXECUTE (100120), is also described but was never implemented, and the
   E/F-series microcode execute a NOP for this instruction code.

   Notes:

     1. Under simulation, TIMER, DIAG, and EXECUTE cause undefined instruction
        stops if the CPU is set to 21xx.  DIAG and EXECUTE also cause stops on
        the 1000-M.  TIMER does not, because it is used by several HP programs
        to differentiate between M- and E/F-series machines.

     2. DIAG is not implemented under simulation.  On the E/F, it performs a
        destructive test of all installed memory.  Because of this, it is only
        functional if the machine is halted, i.e., if the instruction is
        executed with the INSTR STEP button.  If it is executed in a program,
        the result is NOP.

     3. RRR is permitted and executed as NOP if the CPU is a 2114, as the
        presence of the EAU is tested by the diagnostic configurator to
        differentiate between 2114 and 2100/1000 CPUs.
*/

t_stat cpu_eau (uint32 IR, uint32 intrq)
{
t_stat reason = SCPE_OK;
OPS op;
uint32 rs, qs, sc, v1, v2, t;
int32 sop1, sop2;

if ((cpu_unit.flags & UNIT_EAU) == 0)                   /* option installed? */
    if ((UNIT_CPU_MODEL == UNIT_2114) && (IR == 0101100))   /* 2114 and RRR 16? */
        return SCPE_OK;                                 /* allowed as NOP */
    else
        return stop_inst;                               /* fail */

switch ((IR >> 8) & 0377) {                             /* decode IR<15:8> */

    case 0200:                                          /* EAU group 0 */
        switch ((IR >> 4) & 017) {                      /* decode IR<7:4> */

        case 000:                                       /* DIAG 100000 */
            if (UNIT_CPU_MODEL != UNIT_1000_E)          /* must be 1000-E */
                return stop_inst;                       /* trap if not */
            break;                                      /* DIAG is NOP unless halted */

        case 001:                                       /* ASL 100020-100037 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            O = 0;                                      /* clear ovflo */
            while (sc-- != 0) {                         /* bit by bit */
                t = BR << 1;                            /* shift B */
                BR = (BR & SIGN) | (t & 077777) | (AR >> 15);
                AR = (AR << 1) & DMASK;
                if ((BR ^ t) & SIGN) O = 1;
                }
            break;

        case 002:                                       /* LSL 100040-100057 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            BR = ((BR << sc) | (AR >> (16 - sc))) & DMASK;
            AR = (AR << sc) & DMASK;                    /* BR'AR lsh left */
            break;

        case 003:                                       /* TIMER 100060 */
            if (UNIT_CPU_TYPE != UNIT_TYPE_1000)        /* must be 1000 */
                return stop_inst;                       /* trap if not */
            if (UNIT_CPU_MODEL == UNIT_1000_M)          /* 1000 M-series? */
                goto MPY;                               /* decode as MPY */
            BR = (BR + 1) & DMASK;                      /* increment B */
            if (BR) PC = err_PC;                        /* if !=0, repeat */
            break;

        case 004:                                       /* RRL 100100-100117 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            t = BR;                                     /* BR'AR rot left */
            BR = ((BR << sc) | (AR >> (16 - sc))) & DMASK;
            AR = ((AR << sc) | (t >> (16 - sc))) & DMASK;
            break;

        case 010:                                       /* MPY 100200 (OP_K) */
        MPY:
            if (reason = cpu_ops (OP_K, op, intrq))     /* get operand */
                break;
            sop1 = SEXT (AR);                           /* sext AR */
            sop2 = SEXT (op[0].word);                   /* sext mem */
            sop1 = sop1 * sop2;                         /* signed mpy */
            BR = (sop1 >> 16) & DMASK;                  /* to BR'AR */
            AR = sop1 & DMASK;
            O = 0;                                      /* no overflow */
            break;

        default:                                        /* others undefined */
            return stop_inst;
            }

        break;

    case 0201:                                          /* DIV 100400 (OP_K) */
        if (reason = cpu_ops (OP_K, op, intrq))         /* get operand */
            break;
        if (rs = qs = BR & SIGN) {                      /* save divd sign, neg? */
            AR = (~AR + 1) & DMASK;                     /* make B'A pos */
            BR = (~BR + (AR == 0)) & DMASK;             /* make divd pos */
            }
        v2 = op[0].word;                                /* divr = mem */
        if (v2 & SIGN) {                                /* neg? */
            v2 = (~v2 + 1) & DMASK;                     /* make divr pos */
            qs = qs ^ SIGN;                             /* sign of quotient */
            }
        if (BR >= v2) O = 1;                            /* divide work? */
        else {                                          /* maybe... */
            O = 0;                                      /* assume ok */
            v1 = (BR << 16) | AR;                       /* 32b divd */
            AR = (v1 / v2) & DMASK;                     /* quotient */
            BR = (v1 % v2) & DMASK;                     /* remainder */
            if (AR) {                                   /* quotient > 0? */
                if (qs) AR = (~AR + 1) & DMASK;         /* apply quo sign */
                if ((AR ^ qs) & SIGN) O = 1;            /* still wrong? ovflo */
                }
            if (rs) BR = (~BR + 1) & DMASK;             /* apply rem sign */
            }
        break;

    case 0202:                                          /* EAU group 2 */
        switch ((IR >> 4) & 017) {                      /* decode IR<7:4> */

        case 001:                                       /* ASR 101020-101037 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            AR = ((BR << (16 - sc)) | (AR >> sc)) & DMASK;
            BR = (SEXT (BR) >> sc) & DMASK;             /* BR'AR ash right */
            O = 0;
            break;

        case 002:                                       /* LSR 101040-101057 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            AR = ((BR << (16 - sc)) | (AR >> sc)) & DMASK;
            BR = BR >> sc;                              /* BR'AR log right */
            break;

        case 004:                                       /* RRR 101100-101117 */
            sc = (IR & 017)? (IR & 017): 16;            /* get sc */
            t = AR;                                     /* BR'AR rot right */
            AR = ((AR >> sc) | (BR << (16 - sc))) & DMASK;
            BR = ((BR >> sc) | (t << (16 - sc))) & DMASK;
            break;

        default:                                        /* others undefined */
            return stop_inst;
            }

        break;

    case 0210:                                          /* DLD 104200 (OP_D) */
        if (reason = cpu_ops (OP_D, op, intrq))         /* get operand */
            break;
        AR = (op[0].dword >> 16) & DMASK;               /* load AR */
        BR = op[0].dword & DMASK;                       /* load BR */
        break;

    case 0211:                                          /* DST 104400 (OP_A) */
        if (reason = cpu_ops (OP_A, op, intrq))         /* get operand */
            break;
        WriteW (op[0].word, AR);                        /* store AR */
        WriteW ((op[0].word + 1) & VAMASK, BR);         /* store BR */
        break;

    default:                                            /* should never get here */
        return SCPE_IERR;                               /* bad call from cpu_instr */
    }

return reason;
}

/* UIG 0

   The first User Instruction Group (UIG) encodes firmware options for the 2100
   and 1000.  Instruction codes 105000-105377 are assigned to microcode options
   as follows:

     Instructions   Option Name                  2100   1000-M  1000-E  1000-F
     -------------  --------------------------  ------  ------  ------  ------
     105000-105362  2000 I/O Processor           opt      -       -       -
     105000-105137  Floating Point               opt     std     std     std
     105200-105237  Fast FORTRAN Processor       opt     opt     opt     std
     105240-105257  RTE-IVA/B Extended Memory     -       -      opt     opt
     105240-105257  RTE-6/VM Virtual Memory       -       -      opt     opt
     105300-105317  Distributed System            -       -      opt     opt
     105320-105337  Double Integer                -       -      opt      -
     105320-105337  Scientific Instruction Set    -       -       -      std
     105340-105357  RTE-6/VM Operating System     -       -      opt     opt

   Because the 2100 IOP microcode uses the same instruction range as the 2100 FP
   and FFP options, it cannot coexist with them.  To simplify simulation, the
   2100 IOP instructions are remapped to the equivalent 1000 instructions and
   dispatched to the UIG 1 module.

   Note that if the 2100 IOP is installed, the only valid UIG instructions are
   IOP instructions, as the IOP used the full 2100 microcode addressing space.

   The F-Series moved the three-word extended real instructions from the FFP
   range to the base floating-point range and added four-word double real and
   two-word double integer instructions.  The double integer instructions
   occupied some of the vacated extended real instruction codes in the FFP, with
   the rest assigned to the floating-point range.  Consequently, many
   instruction codes for the F-Series are different from the E-Series.

   Notes:

     1. Product 93585A, available from the "Specials" group, added double
        integer microcode to the E-Series.  The instruction codes were different
        from those in the F-Series to avoid conflicting with the E-Series FFP.
        HP manual number 93585-90007 documents the double integer instructions,
        but no copy of this manual has been found.  The Macro/1000 manual
        (92059-090001) lists E-Series double integer instructions as occupying
        the code points of the F-Series Scientific Instruction Set.

     2. To run the double-integer instructions diagnostic in the absence of
        64-bit integer support (and therefore of F-Series simulation), a special
        DBI dispatcher may be enabled by defining ENABLE_DIAG during
        compilation.  This dispatcher will remap the F-Series DBI instructions
        to the E-Series codes, so that the F-Series diagnostic may be run.
        Because several of the F-Series DBI instruction codes replace M/E-Series
        FFP codes, this dispatcher will only operate if FFP is disabled.

        Note that enabling the dispatcher will produce non-standard FP behavior.
        For example, any code in the range 105000-105017 normally would execute
        a FAD instruction.  With the dispatcher enabled, 105014 would execute a
        .DAD, while the other codes would execute a FAD.  Therefore, ENABLE_DIAG
        should only be used to run the diagnostic and is not intended for
        general use.
*/

t_stat cpu_uig_0 (uint32 IR, uint32 intrq, uint32 iotrap)
{
if ((cpu_unit.flags & UNIT_IOP) &&                      /* I/O Processor? */
    (UNIT_CPU_TYPE == UNIT_TYPE_2100))                  /* 2100 CPU? */
    return cpu_iop (IR, intrq);                         /* dispatch to IOP */


#if !defined (HAVE_INT64) && defined (ENABLE_DIAG)      /* DBI diagnostic dispatcher wanted */

if ((cpu_unit.flags & UNIT_FFP) == 0)
    switch (IR & 0377) {
        case 0014:                                      /* .DAD 105014 */
            return cpu_dbi (0105321, intrq);

        case 0034:                                      /* .DSB 105034 */
            return cpu_dbi (0105327, intrq);

        case 0054:                                      /* .DMP 105054 */
            return cpu_dbi (0105322, intrq);

        case 0074:                                      /* .DDI 105074 */
            return cpu_dbi (0105325, intrq);

        case 0114:                                      /* .DSBR 105114 */
            return cpu_dbi (0105334, intrq);

        case 0134:                                      /* .DDIR 105134 */
            return cpu_dbi (0105326, intrq);

        case 0203:                                      /* .DNG 105203 */
            return cpu_dbi (0105323, intrq);

        case 0204:                                      /* .DCO 105204 */
            return cpu_dbi (0105324, intrq);

        case 0210:                                      /* .DIN 105210 */
            return cpu_dbi (0105330, intrq);

        case 0211:                                      /* .DDE 105211 */
            return cpu_dbi (0105331, intrq);

        case 0212:                                      /* .DIS 105212 */
            return cpu_dbi (0105332, intrq);

        case 0213:                                      /* .DDS 105213 */
            return cpu_dbi (0105333, intrq);
        }                                               /* otherwise, continue */

#endif                                                  /* end of DBI dispatcher */


switch ((IR >> 4) & 017) {                              /* decode IR<7:4> */

    case 000:                                           /* 105000-105017 */
    case 001:                                           /* 105020-105037 */
    case 002:                                           /* 105040-105057 */
    case 003:                                           /* 105060-105077 */
    case 004:                                           /* 105100-105117 */
    case 005:                                           /* 105120-105137 */
#if defined (HAVE_INT64)                                /* int64 support available */
        return cpu_fpp (IR, intrq);                     /* Floating Point Processor */
#else                                                   /* int64 support unavailable */
        return cpu_fp (IR, intrq);                      /* Firmware Floating Point */
#endif                                                  /* end of int64 support */

    case 010:                                           /* 105200-105217 */
    case 011:                                           /* 105220-105237 */
        return cpu_ffp (IR, intrq);                     /* Fast FORTRAN Processor */

    case 012:                                           /* 105240-105257 */
        return cpu_rte_vma (IR, intrq);                 /* RTE-4/6 EMA/VMA */

    case 014:                                           /* 105300-105317 */
        return cpu_ds (IR, intrq);                      /* Distributed System */

    case 015:                                           /* 105320-105337 */
#if defined (HAVE_INT64)                                /* int64 support available */
        if (UNIT_CPU_MODEL == UNIT_1000_F)              /* F-series? */
            return cpu_sis (IR, intrq);                 /* Scientific Instruction */
        else                                            /* M/E-series */
#endif                                                  /* end of int64 support */
            return cpu_dbi (IR, intrq);                 /* Double integer */

    case 016:                                           /* 105340-105357 */
        return cpu_rte_os (IR, intrq, iotrap);          /* RTE-6 OS */
    }

return stop_inst;                                       /* others undefined */
}

/* UIG 1

   The second User Instruction Group (UIG) encodes firmware options for the
   1000.  Instruction codes 101400-101777 and 105400-105777 are assigned to
   microcode options as follows ("x" is "1" or "5" below):

     Instructions   Option Name                   1000-M  1000-E  1000-F
     -------------  ----------------------------  ------  ------  ------
     10x400-10x437  2000 IOP                       opt     opt      -
     10x460-10x477  2000 IOP                       opt     opt      -
     10x460-10x477  Vector Instruction Set          -       -      opt
     105520-105537  Distributed System             opt      -       -
     105600-105617  SIGNAL/1000 Instruction Set     -       -      opt
     10x700-10x737  Dynamic Mapping System         opt     opt     std
     10x740-10x777  Extended Instruction Group     std     std     std

   Only 1000 systems execute these instructions.
*/

t_stat cpu_uig_1 (uint32 IR, uint32 intrq, uint32 iotrap)
{
if (UNIT_CPU_TYPE != UNIT_TYPE_1000)                    /* 1000 execution? */
    return stop_inst;                                   /* no, so trap */

switch ((IR >> 4) & 017) {                              /* decode IR<7:4> */

    case 000:                                           /* 105400-105417 */
    case 001:                                           /* 105420-105437 */
        return cpu_iop (IR, intrq);                     /* 2000 I/O Processor */

    case 003:                                           /* 105460-105477 */
        if (UNIT_CPU_MODEL == UNIT_1000_F)              /* F-series? */
            return cpu_vis (IR, intrq);                 /* Vector Instruction Set */
        else                                            /* M/E-series */
            return cpu_iop (IR, intrq);                 /* 2000 I/O Processor */

    case 005:                                           /* 105520-105537 */
        IR = IR ^ 0000620;                              /* remap to 105300-105317 */
        return cpu_ds (IR, intrq);                      /* Distributed System */

    case 010:                                           /* 105600-105617 */
        return cpu_signal (IR, intrq);                  /* SIGNAL/1000 Instructions */

    case 014:                                           /* 105700-105717 */
    case 015:                                           /* 105720-105737 */
        return cpu_dms (IR, intrq);                     /* Dynamic Mapping System */

    case 016:                                           /* 105740-105737 */
    case 017:                                           /* 105760-105777 */
        return cpu_eig (IR, intrq);                     /* Extended Instruction Group */
    }

return stop_inst;                                       /* others undefined */
}


/* Read a multiple-precision operand value. */

OP ReadOp (uint32 va, OPSIZE precision)
{
OP operand;
uint32 i;

if (precision == in_s)
    operand.word = ReadW (va);                          /* read single integer */

else if (precision == in_d)
    operand.dword = ReadW (va) << 16 |                  /* read double integer */
                    ReadW ((va + 1) & VAMASK);          /* merge high and low words */

else
    for (i = 0; i < (uint32) precision; i++) {          /* read fp 2 to 5 words */
        operand.fpk[i] = ReadW (va);
        va = (va + 1) & VAMASK;
        }
return operand;
}

/* Write a multiple-precision operand value. */

void WriteOp (uint32 va, OP operand, OPSIZE precision)
{
uint32 i;

if (precision == in_s)
    WriteW (va, operand.word);                          /* write single integer */

else if (precision == in_d) {
    WriteW (va, (operand.dword >> 16) & DMASK);         /* write double integer */
    WriteW ((va + 1) & VAMASK, operand.dword & DMASK);  /* high word, then low word */
    }

else
    for (i = 0; i < (uint32) precision; i++) {          /* write fp 2 to 5 words */
        WriteW (va, operand.fpk[i]);
        va = (va + 1) & VAMASK;
        }
return;
}


/* Get instruction operands.

   Operands for a given instruction are specifed by an "operand pattern"
   consisting of flags indicating the types and storage methods.  The pattern
   directs how each operand is to be retrieved and whether the operand value or
   address is returned in the operand array.

   Typically, a microcode simulation handler will define an OP_PAT array, with
   each element containing an operand pattern corresponding to the simulated
   instruction.  Operand patterns are defined in the header file accompanying
   this source file.  After calling this function with the appropriate operand
   pattern and a pointer to an array of OPs, operands are decoded and stored
   sequentially in the array.

   The following operand encodings are defined:

      Code   Operand Description                         Example    Return
     ------  ----------------------------------------  -----------  ------------
     OP_NUL  No operand present                           [inst]    None

     OP_IAR  Integer constant in A register                LDA I    Value of I
                                                          [inst]
                                                           ...
                                                        I  DEC 0

     OP_DAB  Double integer constant in A/B registers      DLD J    Value of J
                                                          [inst]
                                                           ...
                                                        J  DEC 0,0

     OP_FAB  2-word FP constant in A/B registers           DLD F    Value of F
                                                          [inst]
                                                           ...
                                                        F  DEC 0.0

     OP_CON  Inline 1-word constant                       [inst]    Value of C
                                                        C  DEC 0
                                                           ...

     OP_VAR  Inline 1-word variable                       [inst]    Address of V
                                                        V  BSS 1
                                                           ...

     OP_ADR  Inline address                               [inst]    Address of A
                                                           DEF A
                                                           ...
                                                        A  EQU *

     OP_ADK  Address of integer constant                  [inst]    Value of K
                                                           DEF K
                                                           ...
                                                        K  DEC 0

     OP_ADD  Address of double integer constant           [inst]    Value of D
                                                           DEF D
                                                           ...
                                                        D  DEC 0,0

     OP_ADF  Address of 2-word FP constant                [inst]    Value of F
                                                           DEF F
                                                           ...
                                                        F  DEC 0.0

     OP_ADX  Address of 3-word FP constant                [inst]    Value of X
                                                           DEF X
                                                           ...
                                                        X  DEX 0.0

     OP_ADT  Address of 4-word FP constant                [inst]    Value of T
                                                           DEF T
                                                           ...
                                                        T  DEY 0.0

     OP_ADE  Address of 5-word FP constant                [inst]    Value of E
                                                           DEF E
                                                           ...
                                                        E  DEC 0,0,0,0,0

   Address operands, i.e., those having a DEF to the operand, will be resolved
   to direct addresses.  If an interrupt is pending and more than three levels
   of indirection are used, the routine returns without completing operand
   retrieval (the instruction will be retried after interrupt servicing).
   Addresses are always resolved in the current DMS map.

   An operand pattern consists of one or more operand encodings, corresponding
   to the operands required by a given instruction.  Values are returned in
   sequence to the operand array.
*/

t_stat cpu_ops (OP_PAT pattern, OPS op, uint32 irq)
{
t_stat reason = SCPE_OK;
OP_PAT flags;
uint32 i, MA;

for (i = 0; i < OP_N_F; i++) {
    flags = pattern & OP_M_FLAGS;                       /* get operand pattern */

    if (flags >= OP_ADR)                                /* address operand? */
        if (reason = resolve (ReadW (PC), &MA, irq))    /* resolve indirects */
            return reason;

    switch (flags) {
        case OP_NUL:                                    /* null operand */
            return reason;                              /* no more, so quit */

        case OP_IAR:                                    /* int in A */
            (*op++).word = AR;                          /* get one-word value */
            break;

        case OP_JAB:                                    /* dbl-int in A/B */
            (*op++).dword = (AR << 16) | BR;            /* get two-word value */
            break;

        case OP_FAB:                                    /* 2-word FP in A/B */
            (*op).fpk[0] = AR;                          /* get high FP word */
            (*op++).fpk[1] = BR;                        /* get low FP word */
            break;

        case OP_CON:                                    /* inline constant operand */
            *op++ = ReadOp (PC, in_s);                  /* get value */
            break;

        case OP_VAR:                                    /* inline variable operand */
            (*op++).word = PC;                          /* get pointer to variable */
            break;

        case OP_ADR:                                    /* inline address operand */
            (*op++).word = MA;                          /* get address */
            break;

        case OP_ADK:                                    /* address of int constant */
            *op++ = ReadOp (MA, in_s);                  /* get value */
            break;

        case OP_ADD:                                    /* address of dbl-int constant */
            *op++ = ReadOp (MA, in_d);                  /* get value */
            break;

        case OP_ADF:                                    /* address of 2-word FP const */
            *op++ = ReadOp (MA, fp_f);                  /* get value */
            break;

        case OP_ADX:                                    /* address of 3-word FP const */
            *op++ = ReadOp (MA, fp_x);                  /* get value */
            break;

        case OP_ADT:                                    /* address of 4-word FP const */
            *op++ = ReadOp (MA, fp_t);                  /* get value */
            break;

        case OP_ADE:                                    /* address of 5-word FP const */
            *op++ = ReadOp (MA, fp_e);                  /* get value */
            break;

        default:
            return SCPE_IERR;                           /* not implemented */
        }

    if (flags >= OP_CON)                                /* operand after instruction? */
        PC = (PC + 1) & VAMASK;                         /* yes, so bump to next */
    pattern = pattern >> OP_N_FLAGS;                    /* move next pattern into place */
    }
return reason;
}
