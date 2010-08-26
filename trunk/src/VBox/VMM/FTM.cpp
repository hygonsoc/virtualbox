/* $Id$ */
/** @file
 * FTM - Fault Tolerance Manager
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_FTM
#include "FTMInternal.h"
#include <VBox/vm.h>
#include <VBox/vmm.h>
#include <VBox/err.h>
#include <VBox/param.h>
#include <VBox/ssm.h>
#include <VBox/log.h>
#include <VBox/pgm.h>

#include <iprt/assert.h>
#include <iprt/thread.h>
#include <iprt/string.h>
#include <iprt/mem.h>
#include <iprt/tcp.h>
#include <iprt/socket.h>
#include <iprt/semaphore.h>
#include <iprt/asm.h>

/*******************************************************************************
 * Structures and Typedefs                                                     *
 *******************************************************************************/

/**
 * TCP stream header.
 *
 * This is an extra layer for fixing the problem with figuring out when the SSM
 * stream ends.
 */
typedef struct FTMTCPHDR
{
    /** Magic value. */
    uint32_t    u32Magic;
    /** The size of the data block following this header.
     * 0 indicates the end of the stream, while UINT32_MAX indicates
     * cancelation. */
    uint32_t    cb;
} FTMTCPHDR;
/** Magic value for FTMTCPHDR::u32Magic. (Egberto Gismonti Amin) */
#define FTMTCPHDR_MAGIC       UINT32_C(0x19471205)
/** The max block size. */
#define FTMTCPHDR_MAX_SIZE    UINT32_C(0x00fffff8)

/**
 * TCP stream header.
 *
 * This is an extra layer for fixing the problem with figuring out when the SSM
 * stream ends.
 */
typedef struct FTMTCPHDRMEM
{
    /** Magic value. */
    uint32_t    u32Magic;
    /** Size (Uncompressed) of the pages following the header. */
    uint32_t    cbPageRange;
    /** GC Physical address of the page(s) to sync. */
    RTGCPHYS    GCPhys;
    /** The size of the data block following this header.
     * 0 indicates the end of the stream, while UINT32_MAX indicates
     * cancelation. */
    uint32_t    cb;
} FTMTCPHDRMEM;

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
static const char g_szWelcome[] = "VirtualBox-Fault-Tolerance-Sync-1.0\n";

/**
 * Initializes the FTM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) FTMR3Init(PVM pVM)
{
    /*
     * Assert alignment and sizes.
     */
    AssertCompile(sizeof(pVM->ftm.s) <= sizeof(pVM->ftm.padding));
    AssertCompileMemberAlignment(FTM, CritSect, sizeof(uintptr_t));

    /** @todo saved state for master nodes! */
    pVM->ftm.s.pszAddress               = NULL;
    pVM->ftm.s.pszPassword              = NULL;
    pVM->fFaultTolerantMaster           = false;
    pVM->ftm.s.fIsStandbyNode           = false;
    pVM->ftm.s.standby.hServer          = NIL_RTTCPSERVER;
    pVM->ftm.s.master.hShutdownEvent    = NIL_RTSEMEVENT;
    pVM->ftm.s.hSocket                  = NIL_RTSOCKET;

    /*
     * Initialize the PGM critical section.
     */
    int rc = PDMR3CritSectInit(pVM, &pVM->ftm.s.CritSect, RT_SRC_POS, "FTM");
    AssertRCReturn(rc, rc);

    STAM_REL_REG(pVM, &pVM->ftm.s.StatReceivedMem,               STAMTYPE_COUNTER, "/FT/Received/Mem",                   STAMUNIT_BYTES, "The amount of memory pages that was received.");
    STAM_REL_REG(pVM, &pVM->ftm.s.StatReceivedState,             STAMTYPE_COUNTER, "/FT/Received/State",                 STAMUNIT_BYTES, "The amount of state information that was received.");
    STAM_REL_REG(pVM, &pVM->ftm.s.StatSentMem,                   STAMTYPE_COUNTER, "/FT/Sent/Mem",                       STAMUNIT_BYTES, "The amount of memory pages that was sent.");
    STAM_REL_REG(pVM, &pVM->ftm.s.StatSentState,                 STAMTYPE_COUNTER, "/FT/Sent/State",                     STAMUNIT_BYTES, "The amount of state information that was sent.");

    return VINF_SUCCESS;
}

/**
 * Terminates the FTM.
 *
 * Termination means cleaning up and freeing all resources,
 * the VM itself is at this point powered off or suspended.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) FTMR3Term(PVM pVM)
{
    if (pVM->ftm.s.pszAddress)
        RTMemFree(pVM->ftm.s.pszAddress);
    if (pVM->ftm.s.pszPassword)
        RTMemFree(pVM->ftm.s.pszPassword);
    if (pVM->ftm.s.hSocket != NIL_RTSOCKET)
        RTTcpClientClose(pVM->ftm.s.hSocket);
    if (pVM->ftm.s.standby.hServer)
        RTTcpServerDestroy(pVM->ftm.s.standby.hServer);
    if (pVM->ftm.s.master.hShutdownEvent != NIL_RTSEMEVENT)
        RTSemEventDestroy(pVM->ftm.s.master.hShutdownEvent);

    PDMR3CritSectDelete(&pVM->ftm.s.CritSect);
    return VINF_SUCCESS;
}


static int ftmR3TcpWriteACK(PVM pVM)
{
    int rc = RTTcpWrite(pVM->ftm.s.hSocket, "ACK\n", sizeof("ACK\n") - 1);
    if (RT_FAILURE(rc))
    {
        LogRel(("FTSync: RTTcpWrite(,ACK,) -> %Rrc\n", rc));
    }
    return rc;
}


static int ftmR3TcpWriteNACK(PVM pVM, int32_t rc2, const char *pszMsgText = NULL)
{
    char    szMsg[256];
    size_t  cch;
    if (pszMsgText && *pszMsgText)
    {
        cch = RTStrPrintf(szMsg, sizeof(szMsg), "NACK=%d;%s\n", rc2, pszMsgText);
        for (size_t off = 6; off + 1 < cch; off++)
            if (szMsg[off] == '\n')
                szMsg[off] = '\r';
    }
    else
        cch = RTStrPrintf(szMsg, sizeof(szMsg), "NACK=%d\n", rc2);
    int rc = RTTcpWrite(pVM->ftm.s.hSocket, szMsg, cch);
    if (RT_FAILURE(rc))
        LogRel(("FTSync: RTTcpWrite(,%s,%zu) -> %Rrc\n", szMsg, cch, rc));
    return rc;
}

/**
 * Reads a string from the socket.
 *
 * @returns VBox status code.
 *
 * @param   pState      The teleporter state structure.
 * @param   pszBuf      The output buffer.
 * @param   cchBuf      The size of the output buffer.
 *
 */
static int ftmR3TcpReadLine(PVM pVM, char *pszBuf, size_t cchBuf)
{
    char       *pszStart = pszBuf;
    RTSOCKET    Sock     = pVM->ftm.s.hSocket;

    AssertReturn(cchBuf > 1, VERR_INTERNAL_ERROR);
    *pszBuf = '\0';

    /* dead simple approach. */
    for (;;)
    {
        char ch;
        int rc = RTTcpRead(Sock, &ch, sizeof(ch), NULL);
        if (RT_FAILURE(rc))
        {
            LogRel(("FTSync: RTTcpRead -> %Rrc while reading string ('%s')\n", rc, pszStart));
            return rc;
        }
        if (    ch == '\n'
            ||  ch == '\0')
            return VINF_SUCCESS;
        if (cchBuf <= 1)
        {
            LogRel(("FTSync: String buffer overflow: '%s'\n", pszStart));
            return VERR_BUFFER_OVERFLOW;
        }
        *pszBuf++ = ch;
        *pszBuf = '\0';
        cchBuf--;
    }
}

/**
 * Reads an ACK or NACK.
 *
 * @returns VBox status code.
 * @param   pVM                 The VM to operate on.
 * @param   pszWhich            Which ACK is this this?
 * @param   pszNAckMsg          Optional NACK message.
 */
static int ftmR3TcpReadACK(PVM pVM, const char *pszWhich, const char *pszNAckMsg = NULL)
{
    char szMsg[256];
    int rc = ftmR3TcpReadLine(pVM, szMsg, sizeof(szMsg));
    if (RT_FAILURE(rc))
        return rc;

    if (!strcmp(szMsg, "ACK"))
        return VINF_SUCCESS;

    if (!strncmp(szMsg, "NACK=", sizeof("NACK=") - 1))
    {
        char *pszMsgText = strchr(szMsg, ';');
        if (pszMsgText)
            *pszMsgText++ = '\0';

        int32_t vrc2;
        rc = RTStrToInt32Full(&szMsg[sizeof("NACK=") - 1], 10, &vrc2);
        if (rc == VINF_SUCCESS)
        {
            /*
             * Well formed NACK, transform it into an error.
             */
            if (pszNAckMsg)
            {
                LogRel(("FTSync: %s: NACK=%Rrc (%d)\n", pszWhich, vrc2, vrc2));
                return VERR_INTERNAL_ERROR;
            }

            if (pszMsgText)
            {
                pszMsgText = RTStrStrip(pszMsgText);
                for (size_t off = 0; pszMsgText[off]; off++)
                    if (pszMsgText[off] == '\r')
                        pszMsgText[off] = '\n';

                LogRel(("FTSync: %s: NACK=%Rrc (%d) - '%s'\n", pszWhich, vrc2, vrc2, pszMsgText));
            }
            return VERR_INTERNAL_ERROR_2;
        }

        if (pszMsgText)
            pszMsgText[-1] = ';';
    }
    return VERR_INTERNAL_ERROR_3;
}

/**
 * Submitts a command to the destination and waits for the ACK.
 *
 * @returns VBox status code.
 *
 * @param   pVM                 The VM to operate on.
 * @param   pszCommand          The command.
 * @param   fWaitForAck         Whether to wait for the ACK.
 */
static int ftmR3TcpSubmitCommand(PVM pVM, const char *pszCommand, bool fWaitForAck = true)
{
    int rc = RTTcpSgWriteL(pVM->ftm.s.hSocket, 2, pszCommand, strlen(pszCommand), "\n", sizeof("\n") - 1);
    if (RT_FAILURE(rc))
        return rc;
    if (!fWaitForAck)
        return VINF_SUCCESS;
    return ftmR3TcpReadACK(pVM, pszCommand);
}

/**
 * @copydoc SSMSTRMOPS::pfnWrite
 */
static DECLCALLBACK(int) ftmR3TcpOpWrite(void *pvUser, uint64_t offStream, const void *pvBuf, size_t cbToWrite)
{
    PVM pVM = (PVM)pvUser;

    AssertReturn(cbToWrite > 0, VINF_SUCCESS);
    AssertReturn(cbToWrite < UINT32_MAX, VERR_OUT_OF_RANGE);
    AssertReturn(pVM->fFaultTolerantMaster, VERR_INVALID_HANDLE);

    for (;;)
    {
        FTMTCPHDR Hdr;
        Hdr.u32Magic = FTMTCPHDR_MAGIC;
        Hdr.cb       = RT_MIN((uint32_t)cbToWrite, FTMTCPHDR_MAX_SIZE);
        int rc = RTTcpSgWriteL(pVM->ftm.s.hSocket, 2, &Hdr, sizeof(Hdr), pvBuf, (size_t)Hdr.cb);
        if (RT_FAILURE(rc))
        {
            LogRel(("FTSync/TCP: Write error: %Rrc (cb=%#x)\n", rc, Hdr.cb));
            return rc;
        }
        pVM->ftm.s.syncstate.uOffStream += Hdr.cb;
        if (Hdr.cb == cbToWrite)
            return VINF_SUCCESS;

        /* advance */
        cbToWrite -= Hdr.cb;
        pvBuf = (uint8_t const *)pvBuf + Hdr.cb;
    }
}


/**
 * Selects and poll for close condition.
 *
 * We can use a relatively high poll timeout here since it's only used to get
 * us out of error paths.  In the normal cause of events, we'll get a
 * end-of-stream header.
 *
 * @returns VBox status code.
 *
 * @param   pState          The teleporter state data.
 */
static int ftmR3TcpReadSelect(PVM pVM)
{
    int rc;
    do
    {
        rc = RTTcpSelectOne(pVM->ftm.s.hSocket, 1000);
        if (RT_FAILURE(rc) && rc != VERR_TIMEOUT)
        {
            pVM->ftm.s.syncstate.fIOError = true;
            LogRel(("FTSync/TCP: Header select error: %Rrc\n", rc));
            break;
        }
        if (pVM->ftm.s.syncstate.fStopReading)
        {
            rc = VERR_EOF;
            break;
        }
    } while (rc == VERR_TIMEOUT);
    return rc;
}


/**
 * @copydoc SSMSTRMOPS::pfnRead
 */
static DECLCALLBACK(int) ftmR3TcpOpRead(void *pvUser, uint64_t offStream, void *pvBuf, size_t cbToRead, size_t *pcbRead)
{
    PVM pVM = (PVM)pvUser;
    AssertReturn(!pVM->fFaultTolerantMaster, VERR_INVALID_HANDLE);

    for (;;)
    {
        int rc;

        /*
         * Check for various conditions and may have been signalled.
         */
        if (pVM->ftm.s.syncstate.fEndOfStream)
            return VERR_EOF;
        if (pVM->ftm.s.syncstate.fStopReading)
            return VERR_EOF;
        if (pVM->ftm.s.syncstate.fIOError)
            return VERR_IO_GEN_FAILURE;

        /*
         * If there is no more data in the current block, read the next
         * block header.
         */
        if (!pVM->ftm.s.syncstate.cbReadBlock)
        {
            rc = ftmR3TcpReadSelect(pVM);
            if (RT_FAILURE(rc))
                return rc;
            FTMTCPHDR Hdr;
            rc = RTTcpRead(pVM->ftm.s.hSocket, &Hdr, sizeof(Hdr), NULL);
            if (RT_FAILURE(rc))
            {
                pVM->ftm.s.syncstate.fIOError = true;
                LogRel(("FTSync/TCP: Header read error: %Rrc\n", rc));
                return rc;
            }

            if (RT_UNLIKELY(   Hdr.u32Magic != FTMTCPHDR_MAGIC
                            || Hdr.cb > FTMTCPHDR_MAX_SIZE
                            || Hdr.cb == 0))
            {
                if (    Hdr.u32Magic == FTMTCPHDR_MAGIC
                    &&  (   Hdr.cb == 0
                         || Hdr.cb == UINT32_MAX)
                   )
                {
                    pVM->ftm.s.syncstate.fEndOfStream = true;
                    pVM->ftm.s.syncstate.cbReadBlock  = 0;
                    return Hdr.cb ? VERR_SSM_CANCELLED : VERR_EOF;
                }
                pVM->ftm.s.syncstate.fIOError = true;
                LogRel(("FTSync/TCP: Invalid block: u32Magic=%#x cb=%#x\n", Hdr.u32Magic, Hdr.cb));
                return VERR_IO_GEN_FAILURE;
            }

            pVM->ftm.s.syncstate.cbReadBlock = Hdr.cb;
            if (pVM->ftm.s.syncstate.fStopReading)
                return VERR_EOF;
        }

        /*
         * Read more data.
         */
        rc = ftmR3TcpReadSelect(pVM);
        if (RT_FAILURE(rc))
            return rc;
        uint32_t cb = (uint32_t)RT_MIN(pVM->ftm.s.syncstate.cbReadBlock, cbToRead);
        rc = RTTcpRead(pVM->ftm.s.hSocket, pvBuf, cb, pcbRead);
        if (RT_FAILURE(rc))
        {
            pVM->ftm.s.syncstate.fIOError = true;
            LogRel(("FTSync/TCP: Data read error: %Rrc (cb=%#x)\n", rc, cb));
            return rc;
        }
        if (pcbRead)
        {
            cb = (uint32_t)*pcbRead;
            pVM->ftm.s.syncstate.uOffStream   += cb;
            pVM->ftm.s.syncstate.cbReadBlock -= cb;
            return VINF_SUCCESS;
        }
        pVM->ftm.s.syncstate.uOffStream   += cb;
        pVM->ftm.s.syncstate.cbReadBlock -= cb;
        if (cbToRead == cb)
            return VINF_SUCCESS;

        /* Advance to the next block. */
        cbToRead -= cb;
        pvBuf = (uint8_t *)pvBuf + cb;
    }
}


/**
 * @copydoc SSMSTRMOPS::pfnSeek
 */
static DECLCALLBACK(int) ftmR3TcpOpSeek(void *pvUser, int64_t offSeek, unsigned uMethod, uint64_t *poffActual)
{
    return VERR_NOT_SUPPORTED;
}


/**
 * @copydoc SSMSTRMOPS::pfnTell
 */
static DECLCALLBACK(uint64_t) ftmR3TcpOpTell(void *pvUser)
{
    PVM pVM = (PVM)pvUser;
    return pVM->ftm.s.syncstate.uOffStream;
}


/**
 * @copydoc SSMSTRMOPS::pfnSize
 */
static DECLCALLBACK(int) ftmR3TcpOpSize(void *pvUser, uint64_t *pcb)
{
    return VERR_NOT_SUPPORTED;
}


/**
 * @copydoc SSMSTRMOPS::pfnIsOk
 */
static DECLCALLBACK(int) ftmR3TcpOpIsOk(void *pvUser)
{
    PVM pVM = (PVM)pvUser;

    if (pVM->fFaultTolerantMaster)
    {
        /* Poll for incoming NACKs and errors from the other side */
        int rc = RTTcpSelectOne(pVM->ftm.s.hSocket, 0);
        if (rc != VERR_TIMEOUT)
        {
            if (RT_SUCCESS(rc))
            {
                LogRel(("FTSync/TCP: Incoming data detect by IsOk, assuming it is a cancellation NACK.\n"));
                rc = VERR_SSM_CANCELLED;
            }
            else
                LogRel(("FTSync/TCP: RTTcpSelectOne -> %Rrc (IsOk).\n", rc));
            return rc;
        }
    }

    return VINF_SUCCESS;
}


/**
 * @copydoc SSMSTRMOPS::pfnClose
 */
static DECLCALLBACK(int) ftmR3TcpOpClose(void *pvUser, bool fCanceled)
{
    PVM pVM = (PVM)pvUser;

    if (pVM->fFaultTolerantMaster)
    {
        FTMTCPHDR EofHdr;
        EofHdr.u32Magic = FTMTCPHDR_MAGIC;
        EofHdr.cb       = fCanceled ? UINT32_MAX : 0;
        int rc = RTTcpWrite(pVM->ftm.s.hSocket, &EofHdr, sizeof(EofHdr));
        if (RT_FAILURE(rc))
        {
            LogRel(("FTSync/TCP: EOF Header write error: %Rrc\n", rc));
            return rc;
        }
    }
    else
    {
        ASMAtomicWriteBool(&pVM->ftm.s.syncstate.fStopReading, true);
    }

    return VINF_SUCCESS;
}


/**
 * Method table for a TCP based stream.
 */
static SSMSTRMOPS const g_ftmR3TcpOps =
{
    SSMSTRMOPS_VERSION,
    ftmR3TcpOpWrite,
    ftmR3TcpOpRead,
    ftmR3TcpOpSeek,
    ftmR3TcpOpTell,
    ftmR3TcpOpSize,
    ftmR3TcpOpIsOk,
    ftmR3TcpOpClose,
    SSMSTRMOPS_VERSION
};

/**
 * Sync the VM state partially or fully
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 * @param   enmState    Which state to sync
 */
static DECLCALLBACK(void) ftmR3PerformSync(PVM pVM, FTMSYNCSTATE enmState)
{
    int rc;

    if (enmState != FTMSYNCSTATE_DELTA_MEMORY)
    {
        rc = VMR3Suspend(pVM);
        AssertReturnVoid(RT_SUCCESS(rc));
    }

    switch (enmState)
    {
    case FTMSYNCSTATE_FULL:
    {
        bool fSuspended = false;

        rc = ftmR3TcpSubmitCommand(pVM, "full-sync");
        AssertRC(rc);

        rc = VMR3Save(pVM, NULL /* pszFilename */, &g_ftmR3TcpOps, pVM, true /* fContinueAfterwards */, NULL, NULL, &fSuspended);
        AssertRC(rc);

        rc = ftmR3TcpReadACK(pVM, "full-sync-complete");
        AssertRC(rc);
        break;
    }

    case FTMSYNCSTATE_DELTA_VM:
        break;

    case FTMSYNCSTATE_DELTA_MEMORY:
        /* Nothing to do as we sync the memory in an async thread; no need to block EMT. */
        break;
    }
    /* Write protect all memory. */
    rc = PGMR3PhysWriteProtectRAM(pVM);
    AssertRC(rc);

    if (enmState != FTMSYNCSTATE_DELTA_MEMORY)
    {
        rc = VMR3Resume(pVM);
        AssertRC(rc);
    }
}

/**
 * PGMR3PhysEnumDirtyFTPages callback for syncing dirty physical pages
 *
 * @param   pVM             VM Handle.
 * @param   GCPhys          GC physical address
 * @param   pRange          HC virtual address of the page(s)
 * @param   cbRange         Size of the dirty range in bytes.
 * @param   pvUser          User argument
 */
static DECLCALLBACK(int) ftmR3SyncDirtyPage(PVM pVM, RTGCPHYS GCPhys, uint8_t *pRange, unsigned cbRange, void *pvUser)
{
    FTMTCPHDRMEM Hdr;
    Hdr.u32Magic    = FTMTCPHDR_MAGIC;
    Hdr.GCPhys      = GCPhys;
    Hdr.cbPageRange = cbRange;
    Hdr.cb          = cbRange;
    /** @todo compress page(s). */
    int rc = RTTcpSgWriteL(pVM->ftm.s.hSocket, 2, &Hdr, sizeof(Hdr), pRange, (size_t)Hdr.cb);
    if (RT_FAILURE(rc))
    {
        LogRel(("FTSync/TCP: Write error (ftmR3SyncDirtyPage): %Rrc (cb=%#x)\n", rc, Hdr.cb));
        return rc;
    }
    return VINF_SUCCESS;
}

/**
 * Thread function which starts syncing process for this master VM
 *
 * @param   Thread      The thread id.
 * @param   pvUser      Not used
 * @return  VINF_SUCCESS (ignored).
 *
 */
static DECLCALLBACK(int) ftmR3MasterThread(RTTHREAD Thread, void *pvUser)
{
    int rc  = VINF_SUCCESS;
    PVM pVM = (PVM)pvUser;

    for (;;)
    {
        /*
         * Try connect to the standby machine.
         */
        rc = RTTcpClientConnect(pVM->ftm.s.pszAddress, pVM->ftm.s.uPort, &pVM->ftm.s.hSocket);
        if (RT_SUCCESS(rc))
        {
            /* Disable Nagle. */
            rc = RTTcpSetSendCoalescing(pVM->ftm.s.hSocket, false /*fEnable*/);
            AssertRC(rc);

            /* Read and check the welcome message. */
            char szLine[RT_MAX(128, sizeof(g_szWelcome))];
            RT_ZERO(szLine);
            rc = RTTcpRead(pVM->ftm.s.hSocket, szLine, sizeof(g_szWelcome) - 1, NULL);
            if (    RT_SUCCESS(rc)
                &&  !strcmp(szLine, g_szWelcome))
            {
                /* password */
                rc = RTTcpWrite(pVM->ftm.s.hSocket, pVM->ftm.s.pszPassword, strlen(pVM->ftm.s.pszPassword));
                if (RT_SUCCESS(rc))
                {
                    /* ACK */
                    rc = ftmR3TcpReadACK(pVM, "password", "Invalid password");
                    if (RT_SUCCESS(rc))
                    {
                        /** todo: verify VM config. */
                        break;
                    }
                }
            }
            rc = RTTcpClientClose(pVM->ftm.s.hSocket);
            AssertRC(rc);
            pVM->ftm.s.hSocket = NIL_RTSOCKET;
        }
        rc = RTSemEventWait(pVM->ftm.s.master.hShutdownEvent, 1000 /* 1 second */);
        if (rc != VERR_TIMEOUT)
            return VINF_SUCCESS;    /* told to quit */            
    }

    /* Successfully initialized the connection to the standby node.
     * Start the sync process.
     */

    /* First sync all memory and write protect everything so 
     * we can send changed pages later on.
     */

    rc = VMR3ReqCallWait(pVM, VMCPUID_ANY, (PFNRT)ftmR3PerformSync, 2, pVM, FTMSYNCSTATE_FULL);
    AssertRC(rc);

    for (;;)
    {
        rc = RTSemEventWait(pVM->ftm.s.master.hShutdownEvent, pVM->ftm.s.uInterval);
        if (rc != VERR_TIMEOUT)
            break;    /* told to quit */

        if (!pVM->ftm.s.fCheckpointingActive)
        {
            rc = PDMCritSectEnter(&pVM->ftm.s.CritSect, VERR_SEM_BUSY);
            AssertMsg(rc == VINF_SUCCESS, ("%Rrc\n", rc));

            rc = ftmR3TcpSubmitCommand(pVM, "mem-sync");
            AssertRC(rc);

            /* sync the changed memory with the standby node. */
            rc = VMR3ReqCallWait(pVM, VMCPUID_ANY, (PFNRT)ftmR3PerformSync, 2, pVM, FTMSYNCSTATE_DELTA_MEMORY);
            AssertRC(rc);

            /* Enumerate all dirty pages and send them to the standby VM. */
            rc = PGMR3PhysEnumDirtyFTPages(pVM, ftmR3SyncDirtyPage, NULL /* pvUser */);
            AssertRC(rc);

            /* Send last memory header to signal the end. */
            FTMTCPHDRMEM Hdr;
            Hdr.u32Magic    = FTMTCPHDR_MAGIC;
            Hdr.GCPhys      = 0;
            Hdr.cbPageRange = 0;
            Hdr.cb          = 0;
            int rc = RTTcpSgWriteL(pVM->ftm.s.hSocket, 1, &Hdr, sizeof(Hdr));
            if (RT_FAILURE(rc))
                LogRel(("FTSync/TCP: Write error (ftmR3MasterThread): %Rrc (cb=%#x)\n", rc, Hdr.cb));

            rc = ftmR3TcpReadACK(pVM, "mem-sync-complete");
            AssertRC(rc);

            PDMCritSectLeave(&pVM->ftm.s.CritSect);
        }
    }
    return rc;
}

/**
 * Listen for incoming traffic destined for the standby VM.
 *
 * @copydoc FNRTTCPSERVE
 *
 * @returns VINF_SUCCESS or VERR_TCP_SERVER_STOP.
 */
static DECLCALLBACK(int) ftmR3StandbyServeConnection(RTSOCKET Sock, void *pvUser)
{
    PVM pVM = (PVM)pvUser;

    pVM->ftm.s.hSocket = Sock;

    /*
     * Disable Nagle.
     */
    int rc = RTTcpSetSendCoalescing(Sock, false /*fEnable*/);
    AssertRC(rc);

    /* Send the welcome message to the master node. */
    rc = RTTcpWrite(Sock, g_szWelcome, sizeof(g_szWelcome) - 1);
    if (RT_FAILURE(rc))
    {
        LogRel(("Teleporter: Failed to write welcome message: %Rrc\n", rc));
        return VINF_SUCCESS;
    }

    /*
     * Password.
     */
    const char *pszPassword = pVM->ftm.s.pszPassword;
    unsigned    off = 0;
    while (pszPassword[off])
    {
        char ch;
        rc = RTTcpRead(Sock, &ch, sizeof(ch), NULL);
        if (    RT_FAILURE(rc)
            ||  pszPassword[off] != ch)
        {
            if (RT_FAILURE(rc))
                LogRel(("FTSync: Password read failure (off=%u): %Rrc\n", off, rc));
            else
                LogRel(("FTSync: Invalid password (off=%u)\n", off));
            ftmR3TcpWriteNACK(pVM, VERR_AUTHENTICATION_FAILURE);
            return VINF_SUCCESS;
        }
        off++;
    }
    rc = ftmR3TcpWriteACK(pVM);
    if (RT_FAILURE(rc))
        return VINF_SUCCESS;

    /** todo: verify VM config. */

    /*
     * Stop the server.
     *
     * Note! After this point we must return VERR_TCP_SERVER_STOP, while prior
     *       to it we must not return that value!
     */
    RTTcpServerShutdown(pVM->ftm.s.standby.hServer);

    /*
     * Command processing loop.
     */
    bool fDone = false;
    for (;;)
    {
        char szCmd[128];
        rc = ftmR3TcpReadLine(pVM, szCmd, sizeof(szCmd));
        AssertRC(rc);
        if (RT_FAILURE(rc))
            break;

        if (!strcmp(szCmd, "mem-sync"))
        {
            rc = ftmR3TcpWriteACK(pVM);
            AssertRC(rc);
            if (RT_FAILURE(rc))
                continue;

            while (true)
            {
                FTMTCPHDRMEM Hdr;
                void *pPage;

                /* Read memory header. */
                rc = RTTcpRead(pVM->ftm.s.hSocket, &Hdr, sizeof(Hdr), NULL);
                if (RT_FAILURE(rc))
                {
                    Log(("RTTcpRead failed with %Rrc\n", rc));
                    break;
                }

                if (Hdr.cb == 0)
                    break;  /* end of sync. */

                Assert(Hdr.cb == Hdr.cbPageRange);  /** @todo uncompress */

                /* Allocate memory to hold the page(s). */
                pPage = RTMemAlloc(Hdr.cbPageRange);
                AssertBreak(pPage);

                /* Fetch the page(s). */
                rc = RTTcpRead(pVM->ftm.s.hSocket, pPage, Hdr.cb, NULL);
                if (RT_FAILURE(rc))
                {
                    Log(("RTTcpRead page data (%d bytes) failed with %Rrc\n", Hdr.cb, rc));
                    break;
                }

                /* Update the guest memory of the standby VM. */
                rc = PGMPhysWrite(pVM, Hdr.GCPhys, pPage, Hdr.cbPageRange);
                AssertRC(rc);

                RTMemFree(pPage);
            }

            rc = ftmR3TcpWriteACK(pVM);
            AssertRC(rc);
        }
        else
        if (!strcmp(szCmd, "heartbeat"))
        {
        }
        else
        if (!strcmp(szCmd, "checkpoint"))
        {
        }
        else
        if (!strcmp(szCmd, "full-sync"))
        {
            rc = ftmR3TcpWriteACK(pVM);
            AssertRC(rc);
            if (RT_FAILURE(rc))
                continue;

            RTSocketRetain(pVM->ftm.s.hSocket); /* For concurrent access by I/O thread and EMT. */
            pVM->ftm.s.syncstate.uOffStream = 0;

            rc = VMR3LoadFromStream(pVM, &g_ftmR3TcpOps, pVM, NULL, NULL);
            RTSocketRelease(pVM->ftm.s.hSocket);
            AssertRC(rc);
            if (RT_FAILURE(rc))
            {
                LogRel(("FTSync: VMR3LoadFromStream -> %Rrc\n", rc));
                ftmR3TcpWriteNACK(pVM, rc);
                continue;
            }

            /* The EOS might not have been read, make sure it is. */
            pVM->ftm.s.syncstate.fStopReading = false;
            size_t cbRead;
            rc = ftmR3TcpOpRead(pVM, pVM->ftm.s.syncstate.uOffStream, szCmd, 1, &cbRead);
            if (rc != VERR_EOF)
            {
                LogRel(("FTSync: Draining teleporterTcpOpRead -> %Rrc\n", rc));
                ftmR3TcpWriteNACK(pVM, rc);
                continue;
            }

            rc = ftmR3TcpWriteACK(pVM);
            AssertRC(rc);
        }        
    }
    LogFlowFunc(("returns mRc=%Rrc\n", rc));
    return VERR_TCP_SERVER_STOP;
}

/**
 * Powers on the fault tolerant virtual machine.
 *
 * @returns VBox status code.
 *
 * @param   pVM         The VM to operate on.
 * @param   fMaster     FT master or standby
 * @param   uInterval   FT sync interval
 * @param   pszAddress  Standby VM address
 * @param   uPort       Standby VM port
 * @param   pszPassword FT password (NULL for none)
 *
 * @thread      Any thread.
 * @vmstate     Created
 * @vmstateto   PoweringOn+Running (master), PoweringOn+Running_FT (standby)
 */
VMMR3DECL(int) FTMR3PowerOn(PVM pVM, bool fMaster, unsigned uInterval, const char *pszAddress, unsigned uPort, const char *pszPassword)
{
    int rc = VINF_SUCCESS;

    VMSTATE enmVMState = VMR3GetState(pVM);
    AssertMsgReturn(enmVMState == VMSTATE_POWERING_ON,
                    ("%s\n", VMR3GetStateName(enmVMState)),
                    VERR_INTERNAL_ERROR_4);
    AssertReturn(pszAddress, VERR_INVALID_PARAMETER);

    if (pVM->ftm.s.uInterval)
        pVM->ftm.s.uInterval    = uInterval;
    else
        pVM->ftm.s.uInterval    = 50;   /* standard sync interval of 50ms */

    pVM->ftm.s.uPort            = uPort;
    pVM->ftm.s.pszAddress       = RTStrDup(pszAddress);
    if (pszPassword)
        pVM->ftm.s.pszPassword  = RTStrDup(pszPassword);
    if (fMaster)
    {
        rc = RTSemEventCreate(&pVM->ftm.s.master.hShutdownEvent);
        if (RT_FAILURE(rc))
            return rc;

        rc = RTThreadCreate(NULL, ftmR3MasterThread, pVM,
                            0, RTTHREADTYPE_IO /* higher than normal priority */, 0, "ftmR3MasterThread");
        if (RT_FAILURE(rc))
            return rc;

        pVM->fFaultTolerantMaster = true;
        if (PGMIsUsingLargePages(pVM))
        {
            /* Must disable large page usage as 2 MB pages are too big to write monitor. */
            LogRel(("FTSync: disabling large page usage.\n"));
            PGMSetLargePageUsage(pVM, false);
        }
        /** @todo might need to disable page fusion as well */

        return VMR3PowerOn(pVM);
    }
    else
    {
        /* standby */
        rc = RTTcpServerCreateEx(pszAddress, uPort, &pVM->ftm.s.standby.hServer);
        if (RT_FAILURE(rc))
            return rc;
        pVM->ftm.s.fIsStandbyNode = true;

        rc = RTTcpServerListen(pVM->ftm.s.standby.hServer, ftmR3StandbyServeConnection, pVM);
        /** @todo deal with the exit code to check if we should activate this standby VM. */

        RTTcpServerDestroy(pVM->ftm.s.standby.hServer);
        pVM->ftm.s.standby.hServer = NULL;
    }
    return rc;
}

/**
 * Powers off the fault tolerant virtual machine (standby).
 *
 * @returns VBox status code.
 *
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) FTMR3CancelStandby(PVM pVM)
{
    AssertReturn(!pVM->fFaultTolerantMaster, VERR_NOT_SUPPORTED);
    Assert(pVM->ftm.s.standby.hServer);

    return RTTcpServerShutdown(pVM->ftm.s.standby.hServer);
}


/**
 * Performs a full sync to the standby node
 *
 * @returns VBox status code.
 *
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) FTMR3SyncState(PVM pVM)
{
    if (!pVM->fFaultTolerantMaster)
        return VINF_SUCCESS;

    pVM->ftm.s.fCheckpointingActive = true;
    int rc = PDMCritSectEnter(&pVM->ftm.s.CritSect, VERR_SEM_BUSY);
    AssertMsg(rc == VINF_SUCCESS, ("%Rrc\n", rc));

    /* Reset the sync state. */
    pVM->ftm.s.syncstate.uOffStream   = 0;
    pVM->ftm.s.syncstate.cbReadBlock  = 0;
    pVM->ftm.s.syncstate.fStopReading = false;
    pVM->ftm.s.syncstate.fIOError     = false;
    pVM->ftm.s.syncstate.fEndOfStream = false;

    /* Sync state + changed memory with the standby node. */
    rc = VMR3ReqCallWait(pVM, VMCPUID_ANY, (PFNRT)ftmR3PerformSync, 2, pVM, FTMSYNCSTATE_DELTA_VM);
    AssertRC(rc);

    PDMCritSectLeave(&pVM->ftm.s.CritSect);
    pVM->ftm.s.fCheckpointingActive = false;

    return VERR_NOT_IMPLEMENTED;
}
