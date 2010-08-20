/* $Id$ */
/** @file
 * IPRT Testcase - Core Dumper.
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
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/types.h>
#include <iprt/file.h>
#include <iprt/err.h>
#include <iprt/dir.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/initterm.h>
#include <iprt/thread.h>
#include <iprt/param.h>
#include <iprt/asm.h>
#include "tstRTCoreDump.h"

#ifdef RT_OS_SOLARIS
# include <signal.h>
# include <unistd.h>
# include <errno.h>
# include <zone.h>
# include <sys/proc.h>
# include <sys/sysmacros.h>
# include <sys/systeminfo.h>
# include <sys/mman.h>
#endif  /* RT_OS_SOLARIS */

/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
#define CORELOG(a)          RTPrintf a
#define CORELOGREL(a)       RTPrintf a

/**
 * VBOXSOLCORETYPE: Whether this is an old or new style core.
 */
typedef enum VBOXSOLCORETYPE
{
    enmOldEra       = 0x01d,        /* old */
    enmNewEra       = 0x5c151       /* sci-fi */
} VBOXSOLCORETYPE;

static unsigned volatile g_cErrors = 0;

volatile bool g_fCoreDumpInProgress = false;


/**
 * Determines endianness of the system. Just for completeness.
 *
 * @return Will return false if system is little endian, true otherwise.
 */
static bool IsBigEndian()
{
    const int i = 1;
    char *p = (char *)&i;
    if (p[0] == 1)
        return false;
    return true;
}


/**
 * Reads from a file making sure an interruption doesn't cause a failure.
 *
 * @param hFile             Handle to the file to read.
 * @param pv                Where to store the read data.
 * @param cbToRead          Size of data to read.
 *
 * @return VBox status code.
 */
static int ReadFileNoIntr(RTFILE hFile, void *pv, size_t cbToRead)
{
    int rc = VERR_READ_ERROR;
    while (1)
    {
        rc = RTFileRead(hFile, pv, cbToRead, NULL /* Read all */);
        if (rc == VERR_INTERRUPTED)
            continue;
        break;
    }
    return rc;
}


/**
 * Writes to a file making sure an interruption doesn't cause a failure.
 *
 * @param hFile             Handle to the file to write.
 * @param pv                Pointer to what to write.
 * @param cbToRead          Size of data to write.
 *
 * @return VBox status code.
 */
static int WriteFileNoIntr(RTFILE hFile, const void *pcv, size_t cbToRead)
{
    int rc = VERR_READ_ERROR;
    while (1)
    {
        rc = RTFileWrite(hFile, pcv, cbToRead, NULL /* Write all */);
        if (rc == VERR_INTERRUPTED)
            continue;
        break;
    }
    return rc;
}


/**
 * Read from a given offet in the process' address space.
 *
 * @param pVBoxProc         Pointer to the VBox process.
 * @param pv                Where to read the data into.
 * @param cb                Size of the read buffer.
 * @param off               Offset to read from.
 *
 * @return VINF_SUCCESS, if all the given bytes was read in, otherwise VERR_READ_ERROR.
 */
static ssize_t ReadProcAddrSpace(PVBOXPROCESS pVBoxProc, RTFOFF off, void *pvBuf, size_t cbToRead)
{
    while (1)
    {
        int rc = RTFileReadAt(pVBoxProc->hAs, off, pvBuf, cbToRead, NULL);
        if (rc == VERR_INTERRUPTED)
            continue;
        return rc;
    }
}


/**
 * Determines if the current process' architecture is suitable for dumping core.
 *
 * @param pVBoxProc         Pointer to the VBox process.
 *
 * @return true if the architecture matches the current one.
 */
inline bool IsProcArchNative(PVBOXPROCESS pVBoxProc)
{
    return pVBoxProc->ProcInfo.pr_dmodel == PR_MODEL_NATIVE;
}


/**
 * Helper function to get the size of a file given it's path.
 *
 * @param pszPath           Pointer to the full path of the file.
 *
 * @return The size of the file in bytes.
 */
size_t GetFileSize(const char *pszPath)
{
    size_t cb = 0;
    RTFILE hFile;
    int rc = RTFileOpen(&hFile, pszPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_SUCCESS(rc))
    {
        RTFileGetSize(hFile, (uint64_t *)&cb);
        RTFileClose(hFile);
    }
    else
        CORELOGREL(("GetFileSize failed to open %s rc=%Rrc\n", pszPath, rc));
    return cb;
}


/**
 * Pre-compute and pre-allocate sufficient memory for dumping core.
 * This is meant to be called once, as a single-large anonymously
 * mapped memory area which will be used during the core dumping routines.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int AllocMemoryArea(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore->pvCore == NULL, VERR_ALREADY_EXISTS);
    AssertReturn(pVBoxCore->VBoxProc.Process != NIL_RTPROCESS, VERR_PROCESS_NOT_FOUND);

    struct VBOXSOLPREALLOCTABLE
    {
        const char *pszFilePath;        /* Proc based path */
        size_t      cbHeader;           /* Size of header */
        size_t      cbEntry;            /* Size of each entry in file */
        size_t      cbAccounting;       /* Size of each accounting entry per entry */
    } aPreAllocTable[] = {
        { "/proc/%d/map",        0,                  sizeof(prmap_t),       sizeof(VBOXSOLMAPINFO) },
        { "/proc/%d/auxv",       0,                  0,                     0 },
        { "/proc/%d/lpsinfo",    sizeof(prheader_t), sizeof(lwpsinfo_t),    sizeof(VBOXSOLTHREADINFO) },
        { "/proc/%d/lstatus",    0,                  0,                     0 },
        { "/proc/%d/ldt",        0,                  0,                     0 },
        { "/proc/%d/cred",       sizeof(prcred_t),   sizeof(gid_t),         1 },
        { "/proc/%d/priv",       sizeof(prpriv_t),   sizeof(priv_chunk_t),  1 },
    };

    size_t cb = 0;
    for (int i = 0; i < (int)RT_ELEMENTS(aPreAllocTable); i++)
    {
        char szPath[PATH_MAX];
        RTStrPrintf(szPath, sizeof(szPath), aPreAllocTable[i].pszFilePath, (int)pVBoxCore->VBoxProc.Process);
        size_t cbFile = GetFileSize(szPath);
        cb += cbFile;
        if (   cbFile > 0
            && aPreAllocTable[i].cbEntry > 0
            && aPreAllocTable[i].cbAccounting > 0)
        {
            cb += ((cbFile - aPreAllocTable[i].cbHeader) / aPreAllocTable[i].cbEntry) * aPreAllocTable[i].cbAccounting;
            cb += aPreAllocTable[i].cbHeader;
        }
    }

    /*
     * Make Room for our own mapping accountant entry which will also be included in the core.
     */
    cb += sizeof(VBOXSOLMAPINFO);

    /*
     * Allocate the required space, plus some extra room.
     */
    cb += _128K;
    void *pv = mmap(NULL, cb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1 /* fd */, 0 /* offset */);
    if (pv)
    {
        CORELOG(("AllocMemoryArea: memory area of %u bytes allocated.\n", cb));
        pVBoxCore->pvCore = pv;
        pVBoxCore->pvFree = pv;
        pVBoxCore->cbCore = cb;
        return VINF_SUCCESS;
    }
    else
    {
        CORELOGREL(("AllocMemoryArea: failed cb=%u\n", cb));
        return VERR_NO_MEMORY;
    }
}


/**
 * Free memory area used by the core object.
 *
 * @param pVBoxCore         Pointer to the core object.
 */
void FreeMemoryArea(PVBOXCORE pVBoxCore)
{
    AssertReturnVoid(pVBoxCore);
    AssertReturnVoid(pVBoxCore->pvCore);
    AssertReturnVoid(pVBoxCore->cbCore > 0);

    munmap(pVBoxCore->pvCore, pVBoxCore->cbCore);
    CORELOG(("FreeMemoryArea: memory area of %u bytes freed.\n", pVBoxCore->cbCore));

    pVBoxCore->pvCore = NULL;
    pVBoxCore->pvFree= NULL;
    pVBoxCore->cbCore = 0;
}


/**
 * Get a chunk from the area of allocated memory.
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param cb                Size of requested chunk.
 *
 * @return Pointer to allocated memory, or NULL on failure.
 */
void *GetMemoryChunk(PVBOXCORE pVBoxCore, size_t cb)
{
    AssertReturn(pVBoxCore, NULL);
    AssertReturn(pVBoxCore->pvCore, NULL);
    AssertReturn(pVBoxCore->pvFree, NULL);

    size_t cbAllocated = (char *)pVBoxCore->pvFree - (char *)pVBoxCore->pvCore;
    if (cbAllocated < pVBoxCore->cbCore)
    {
        char *pb = (char *)pVBoxCore->pvFree;
        pVBoxCore->pvFree = pb + cb;
        return pb;
    }

    return NULL;
}


/**
 * Reads the proc file's content into a newly allocated buffer.
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param pszFileFmt        Only the name of the file to read from (/proc/<pid> will be prepended)
 * @param ppv               Where to store the allocated buffer.
 * @param pcb               Where to store size of the buffer.
 *
 * @return VBox status code.
 */
int ProcReadFileInto(PVBOXCORE pVBoxCore, const char *pszProcFileName, void **ppv, size_t *pcb)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    char szPath[PATH_MAX];
    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/%s", (int)pVBoxCore->VBoxProc.Process, pszProcFileName);
    RTFILE hFile;
    int rc = RTFileOpen(&hFile, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_SUCCESS(rc))
    {
        RTFileGetSize(hFile, (uint64_t *)pcb);
        if (*pcb > 0)
        {
            *ppv = GetMemoryChunk(pVBoxCore, *pcb);
            if (*ppv)
                rc = ReadFileNoIntr(hFile, *ppv, *pcb);
            else
                rc = VERR_NO_MEMORY;
        }
        else
        {
            *pcb =  0;
            *ppv = NULL;
        }
        RTFileClose(hFile);
    }
    else
        CORELOGREL(("ProcReadFileInto: failed to open %s. rc=%Rrc\n", szPath, rc));
    return rc;
}


/**
 * Read process information (format psinfo_t) from /proc.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int ReadProcInfo(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    char szPath[PATH_MAX];
    RTFILE hFile;

    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/psinfo", (int)pVBoxProc->Process);
    int rc = RTFileOpen(&hFile, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_SUCCESS(rc))
    {
        size_t cbProcInfo = sizeof(psinfo_t);
        rc = ReadFileNoIntr(hFile, &pVBoxProc->ProcInfo, cbProcInfo);
    }

    RTFileClose(hFile);
    return rc;
}


/**
 * Read process status (format pstatus_t) from /proc.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int ReadProcStatus(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;

    char szPath[PATH_MAX];
    RTFILE hFile;

    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/status", (int)pVBoxProc->Process);
    int rc = RTFileOpen(&hFile, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_SUCCESS(rc))
    {
        size_t cbRead;
        size_t cbProcStatus = sizeof(pstatus_t);
        AssertCompile(sizeof(pstatus_t) == sizeof(pVBoxProc->ProcStatus));
        rc = ReadFileNoIntr(hFile, &pVBoxProc->ProcStatus, cbProcStatus);
    }
    RTFileClose(hFile);
    return rc;
}


/**
 * Read process credential information (format prcred_t + array of guid_t)
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcCred(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    return ProcReadFileInto(pVBoxCore, "cred", &pVBoxProc->pvCred, &pVBoxProc->cbCred);
}


/**
 * Read process privilege information (format prpriv_t + array of priv_chunk_t)
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcPriv(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    int rc = ProcReadFileInto(pVBoxCore, "priv", (void **)&pVBoxProc->pPriv, &pVBoxProc->cbPriv);
    if (RT_FAILURE(rc))
        return rc;
    pVBoxProc->pcPrivImpl = getprivimplinfo();
    if (!pVBoxProc->pcPrivImpl)
    {
        CORELOGREL(("ReadProcPriv: getprivimplinfo returned NULL.\n"));
        return VERR_INVALID_STATE;
    }
    return rc;
}


/**
 * Read process LDT information (format array of struct ssd) from /proc.
 *
 * @param pVBoxProc         Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcLdt(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    return ProcReadFileInto(pVBoxCore, "ldt", &pVBoxProc->pvLdt, &pVBoxProc->cbLdt);
}


/**
 * Read process auxiliary vectors (format auxv_t) for the process.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcAuxVecs(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    char szPath[PATH_MAX];
    RTFILE hFile = NIL_RTFILE;
    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/auxv", (int)pVBoxProc->Process);
    int rc = RTFileOpen(&hFile, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("ReadProcAuxVecs: RTFileOpen %s failed rc=%Rrc\n", szPath, rc));
        return rc;
    }

    size_t cbAuxFile = 0;
    RTFileGetSize(hFile, (uint64_t *)&cbAuxFile);
    if (cbAuxFile >= sizeof(auxv_t))
    {
        pVBoxProc->pAuxVecs = (auxv_t*)GetMemoryChunk(pVBoxCore, cbAuxFile + sizeof(auxv_t));
        if (pVBoxProc->pAuxVecs)
        {
            rc = ReadFileNoIntr(hFile, pVBoxProc->pAuxVecs, cbAuxFile);
            if (RT_SUCCESS(rc))
            {
                /* Terminate list of vectors */
                pVBoxProc->cAuxVecs = cbAuxFile / sizeof(auxv_t);
                CORELOG(("ReadProcAuxVecs: cbAuxFile=%u auxv_t size %d cAuxVecs=%u\n", cbAuxFile, sizeof(auxv_t), pVBoxProc->cAuxVecs));
                if (pVBoxProc->cAuxVecs > 0)
                {
                    pVBoxProc->pAuxVecs[pVBoxProc->cAuxVecs].a_type = AT_NULL;
                    pVBoxProc->pAuxVecs[pVBoxProc->cAuxVecs].a_un.a_val = 0L;
                    RTFileClose(hFile);
                    return VINF_SUCCESS;
                }
                else
                {
                    CORELOGREL(("ReadProcAuxVecs: Invalid vector count %u\n", pVBoxProc->cAuxVecs));
                    rc = VERR_READ_ERROR;
                }
            }
            else
                CORELOGREL(("ReadProcAuxVecs: ReadFileNoIntr failed. rc=%Rrc cbAuxFile=%u\n", rc, cbAuxFile));

            pVBoxProc->pAuxVecs = NULL;
            pVBoxProc->cAuxVecs = 0;
        }
        else
        {
            CORELOGREL(("ReadProcAuxVecs: no memory for %u bytes\n", cbAuxFile + sizeof(auxv_t)));
            rc = VERR_NO_MEMORY;
        }
    }
    else
        CORELOGREL(("ReadProcAuxVecs: aux file too small %u, expecting %u or more\n", cbAuxFile, sizeof(auxv_t)));

    RTFileClose(hFile);
    return rc;
}


/*
 * Find an element in the process' auxiliary vector.
 */
long GetAuxVal(PVBOXPROCESS pVBoxProc, int Type)
{
    AssertReturn(pVBoxProc, -1);
    if (pVBoxProc->pAuxVecs)
    {
        auxv_t *pAuxVec = pVBoxProc->pAuxVecs;
        for (; pAuxVec->a_type != AT_NULL; pAuxVec++)
        {
            if (pAuxVec->a_type == Type)
                return pAuxVec->a_un.a_val;
        }
    }
    return -1;
}


/**
 * Read the process mappings (format prmap_t array).
 *
 * @param   pVBoxCore           Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcMappings(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    char szPath[PATH_MAX];
    RTFILE hFile = NIL_RTFILE;
    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/map", (int)pVBoxProc->Process);
    int rc = RTFileOpen(&hFile, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_FAILURE(rc))
        return rc;

    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/as", (int)pVBoxProc->Process);
    rc = RTFileOpen(&pVBoxProc->hAs, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_SUCCESS(rc))
    {
        /*
         * Allocate and read all the prmap_t objects from proc.
         */
        size_t cbMapFile = 0;
        RTFileGetSize(hFile, (uint64_t *)&cbMapFile);
        if (cbMapFile >= sizeof(prmap_t))
        {
            prmap_t *pMap = (prmap_t*)GetMemoryChunk(pVBoxCore, cbMapFile);
            if (pMap)
            {
                rc = ReadFileNoIntr(hFile, pMap, cbMapFile);
                if (RT_SUCCESS(rc))
                {
                    pVBoxProc->cMappings = cbMapFile / sizeof(prmap_t);
                    if (pVBoxProc->cMappings > 0)
                    {
                        /*
                         * Allocate for each prmap_t object, a corresponding VBOXSOLMAPINFO object.
                         */
                        pVBoxProc->pMapInfoHead = (PVBOXSOLMAPINFO)GetMemoryChunk(pVBoxCore, pVBoxProc->cMappings * sizeof(VBOXSOLMAPINFO));
                        if (pVBoxProc->pMapInfoHead)
                        {
                            /*
                             * Associate the prmap_t with the mapping info object.
                             */
                            Assert(pVBoxProc->pMapInfoHead == NULL);
                            PVBOXSOLMAPINFO pCur = pVBoxProc->pMapInfoHead;
                            PVBOXSOLMAPINFO pPrev = NULL;
                            for (uint64_t i = 0; i < pVBoxProc->cMappings; i++, pMap++, pCur++)
                            {
                                memcpy(&pCur->pMap, pMap, sizeof(pCur->pMap));
                                if (pPrev)
                                    pPrev->pNext = pCur;

                                pCur->fError = 0;

                                /*
                                 * Make sure we can read the mapping, otherwise mark them to be skipped.
                                 */
                                char achBuf[PAGE_SIZE];
                                uint64_t k = 0;
                                while (k < pCur->pMap.pr_size)
                                {
                                    size_t cb = RT_MIN(sizeof(achBuf), pCur->pMap.pr_size - k);
                                    int rc2 = ReadProcAddrSpace(pVBoxProc, pCur->pMap.pr_vaddr + k, &achBuf, cb);
                                    if (RT_FAILURE(rc2))
                                    {
                                        CORELOGREL(("ReadProcMappings: skipping mapping. vaddr=%#x rc=%Rrc\n", pCur->pMap.pr_vaddr, rc2));

                                        /*
                                         * Instead of storing the actual mapping data which we failed to read, the core
                                         * will contain an errno in place. So we adjust the prmap_t's size field too
                                         * so the program header offsets match.
                                         */
                                        pCur->pMap.pr_size = RT_ALIGN_Z(sizeof(int), 8);
                                        pCur->fError = errno;
                                        if (pCur->fError == 0)  /* huh!? somehow errno got reset? fake one! EFAULT is nice. */
                                            pCur->fError = EFAULT;
                                        break;
                                    }
                                    k += cb;
                                }

                                pPrev = pCur;
                            }
                            if (pPrev)
                                pPrev->pNext = NULL;

                            RTFileClose(hFile);
                            RTFileClose(pVBoxProc->hAs);
                            pVBoxProc->hAs = NIL_RTFILE;
                            CORELOG(("ReadProcMappings: successfully read in %u mappings\n", pVBoxProc->cMappings));
                            return VINF_SUCCESS;
                        }
                        else
                        {
                            CORELOGREL(("ReadProcMappings: GetMemoryChunk failed %u\n", pVBoxProc->cMappings * sizeof(VBOXSOLMAPINFO)));
                            rc = VERR_NO_MEMORY;
                        }
                    }
                    else
                    {
                        CORELOGREL(("ReadProcMappings: Invalid mapping count %u\n", pVBoxProc->cMappings));
                        rc = VERR_READ_ERROR;
                    }
                }
                else
                    CORELOGREL(("ReadProcMappings: FileReadNoIntr failed. rc=%Rrc cbMapFile=%u\n", rc, cbMapFile));
            }
            else
            {
                CORELOGREL(("ReadProcMappings: GetMemoryChunk failed. cbMapFile=%u\n", cbMapFile));
                rc = VERR_NO_MEMORY;
            }
        }

        RTFileClose(pVBoxProc->hAs);
        pVBoxProc->hAs = NIL_RTFILE;
    }
    else
        CORELOGREL(("ReadProcMappings: failed to open %s. rc=%Rrc\n", szPath, rc));

    RTFileClose(hFile);
    return rc;
}


/**
 * Reads the thread information for all threads in the process.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @remarks Should not be called before successful call to @see AllocMemoryArea()
 * @return VBox status code.
 */
int ReadProcThreads(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    AssertReturn(pVBoxProc->pCurThreadCtx, VERR_NO_DATA);

    /*
     * Read the information for threads.
     * Format: prheader_t + array of lwpsinfo_t's.
     */
    size_t cbInfoHdrAndData;
    void *pvInfoHdr = NULL;
    int rc = ProcReadFileInto(pVBoxCore, "lpsinfo", &pvInfoHdr, &cbInfoHdrAndData);
    if (RT_SUCCESS(rc))
    {
        /*
         * Read the status of threads.
         * Format: prheader_t + array of lwpstatus_t's.
         */
        void *pvStatusHdr = NULL;
        size_t cbStatusHdrAndData;
        rc = ProcReadFileInto(pVBoxCore, "lstatus", &pvStatusHdr, &cbStatusHdrAndData);
        if (RT_SUCCESS(rc))
        {
            prheader_t *pInfoHdr   = (prheader_t *)pvInfoHdr;
            prheader_t *pStatusHdr = (prheader_t *)pvStatusHdr;
            lwpstatus_t *pStatus   = (lwpstatus_t *)((uintptr_t)pStatusHdr + sizeof(prheader_t));
            lwpsinfo_t *pInfo      = (lwpsinfo_t *)((uintptr_t)pInfoHdr + sizeof(prheader_t));
            uint64_t cStatus       = pStatusHdr->pr_nent;
            uint64_t cInfo         = pInfoHdr->pr_nent;

            CORELOG(("ReadProcThreads: read info(%u) status(%u), threads:cInfo=%u cStatus=%u\n", cbInfoHdrAndData, cbStatusHdrAndData, cInfo, cStatus));

            /*
             * Minor sanity size check (remember sizeof lwpstatus_t & lwpsinfo_t is <= size in file per entry).
             */
            if (   (cbStatusHdrAndData - sizeof(prheader_t)) % pStatusHdr->pr_entsize == 0
                && (cbInfoHdrAndData - sizeof(prheader_t)) % pInfoHdr->pr_entsize == 0)
            {
                /*
                 * Make sure we have a matching lstatus entry for an lpsinfo entry unless
                 * it is a zombie thread, in which case we will not have a matching lstatus entry.
                 */
                for (; cInfo != 0; cInfo--)
                {
                    if (pInfo->pr_sname != 'Z') /* zombie */
                    {
                        if (   cStatus == 0
                            || pStatus->pr_lwpid != pInfo->pr_lwpid)
                        {
                            CORELOGREL(("ReadProcThreads: cStatus = %u pStatuslwpid=%d infolwpid=%d\n", cStatus, pStatus->pr_lwpid, pInfo->pr_lwpid));
                            rc = VERR_INVALID_STATE;
                            break;
                        }
                        pStatus = (lwpstatus_t *)((uintptr_t)pStatus + pStatusHdr->pr_entsize);
                        cStatus--;
                    }
                    pInfo = (lwpsinfo_t *)((uintptr_t)pInfo + pInfoHdr->pr_entsize);
                }

                if (RT_SUCCESS(rc))
                {
                    /*
                     * Threre can still be more lwpsinfo_t's than lwpstatus_t's, build the
                     * lists accordingly.
                     */
                    pStatus = (lwpstatus_t *)((uintptr_t)pStatusHdr + sizeof(prheader_t));
                    pInfo = (lwpsinfo_t *)((uintptr_t)pInfoHdr + sizeof(prheader_t));
                    cInfo = pInfoHdr->pr_nent;
                    cStatus = pInfoHdr->pr_nent;

                    size_t cbThreadInfo = RT_MAX(cStatus, cInfo) * sizeof(VBOXSOLTHREADINFO);
                    pVBoxProc->pThreadInfoHead = (PVBOXSOLTHREADINFO)GetMemoryChunk(pVBoxCore, cbThreadInfo);
                    if (pVBoxProc->pThreadInfoHead)
                    {
                        PVBOXSOLTHREADINFO pCur = pVBoxProc->pThreadInfoHead;
                        PVBOXSOLTHREADINFO pPrev = NULL;
                        for (uint64_t i = 0; i < cInfo; i++, pCur++)
                        {
                            pCur->Info = *pInfo;
                            if (   pInfo->pr_sname != 'Z'
                                && pInfo->pr_lwpid == pStatus->pr_lwpid)
                            {
                                /*
                                 * Adjust the context of the dumping thread to reflect the context
                                 * when the core dump got initiated before whatever signal caused it.
                                 */
                                if (   pStatus          /* noid droid */
                                    && pStatus->pr_lwpid == (id_t)pVBoxProc->hCurThread)
                                {
                                    AssertCompile(sizeof(pStatus->pr_reg) == sizeof(pVBoxProc->pCurThreadCtx->uc_mcontext.gregs));
                                    AssertCompile(sizeof(pStatus->pr_fpreg) == sizeof(pVBoxProc->pCurThreadCtx->uc_mcontext.fpregs));
                                    memcpy(&pStatus->pr_reg, &pVBoxProc->pCurThreadCtx->uc_mcontext.gregs, sizeof(pStatus->pr_reg));
                                    memcpy(&pStatus->pr_fpreg, &pVBoxProc->pCurThreadCtx->uc_mcontext.fpregs, sizeof(pStatus->pr_fpreg));

                                    AssertCompile(sizeof(pStatus->pr_lwphold) == sizeof(pVBoxProc->pCurThreadCtx->uc_sigmask));
                                    memcpy(&pStatus->pr_lwphold, &pVBoxProc->pCurThreadCtx->uc_sigmask, sizeof(pStatus->pr_lwphold));
                                    pStatus->pr_ustack = (uintptr_t)&pVBoxProc->pCurThreadCtx->uc_stack;

                                    CORELOG(("ReadProcThreads: patched dumper thread context with pre-dump time context.\n"));
                                }

                                pCur->pStatus = pStatus;
                                pStatus = (lwpstatus_t *)((uintptr_t)pStatus + pStatusHdr->pr_entsize);
                            }
                            else
                            {
                                CORELOGREL(("ReadProcThreads: missing status for lwp %d\n", pInfo->pr_lwpid));
                                pCur->pStatus = NULL;
                            }

                            if (pPrev)
                                pPrev->pNext = pCur;
                            pPrev = pCur;
                            pInfo = (lwpsinfo_t *)((uintptr_t)pInfo + pInfoHdr->pr_entsize);
                        }
                        if (pPrev)
                            pPrev->pNext = NULL;

                        CORELOG(("ReadProcThreads: successfully read %u threads.\n", cInfo));
                        pVBoxProc->cThreads = cInfo;
                        return VINF_SUCCESS;
                    }
                    else
                    {
                        CORELOGREL(("ReadProcThreads: GetMemoryChunk failed for %u bytes\n", cbThreadInfo));
                        rc = VERR_NO_MEMORY;
                    }
                }
                else
                    CORELOGREL(("ReadProcThreads: Invalid state information for threads.\n", rc));
            }
            else
            {
                CORELOGREL(("ReadProcThreads: huh!? cbStatusHdrAndData=%u prheader_t=%u entsize=%u\n", cbStatusHdrAndData,
                            sizeof(prheader_t), pStatusHdr->pr_entsize));
                CORELOGREL(("ReadProcThreads: huh!? cbInfoHdrAndData=%u entsize=%u\n", cbInfoHdrAndData, pStatusHdr->pr_entsize));
                rc = VERR_INVALID_STATE;
            }
        }
        else
            CORELOGREL(("ReadProcThreads: ReadFileNoIntr failed for \"lpsinfo\" rc=%Rrc\n", rc));
    }
    else
        CORELOGREL(("ReadProcThreads: ReadFileNoIntr failed for \"lstatus\" rc=%Rrc\n", rc));
    return rc;
}


/**
 * Reads miscellaneous information that is collected as part of a core file.
 * This may include platform name, zone name and other OS-specific information.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int ReadProcMiscInfo(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;

#ifdef RT_OS_SOLARIS
    /*
     * Read the platform name, uname string and zone name.
     */
    int rc = sysinfo(SI_PLATFORM, pVBoxProc->szPlatform, sizeof(pVBoxProc->szPlatform));
    if (rc == -1)
    {
        CORELOGREL(("ReadProcMiscInfo: sysinfo failed. rc=%d errno=%d\n", rc, errno));
        return VERR_GENERAL_FAILURE;
    }
    pVBoxProc->szPlatform[sizeof(pVBoxProc->szPlatform) - 1] = '\0';

    rc = uname(&pVBoxProc->UtsName);
    if (rc == -1)
    {
        CORELOGREL(("ReadProcMiscInfo: uname failed. rc=%d errno=%d\n", rc, errno));
        return VERR_GENERAL_FAILURE;
    }

    rc = getzonenamebyid(pVBoxProc->ProcInfo.pr_zoneid, pVBoxProc->szZoneName, sizeof(pVBoxProc->szZoneName));
    if (rc < 0)
    {
        CORELOGREL(("ReadProcMiscInfo: getzonenamebyid failed. rc=%d errno=%d zoneid=%d\n", rc, errno, pVBoxProc->ProcInfo.pr_zoneid));
        return VERR_GENERAL_FAILURE;
    }
    pVBoxProc->szZoneName[sizeof(pVBoxProc->szZoneName) - 1] = '\0';
    rc = VINF_SUCCESS;

#else
# error Port Me!
#endif
    return rc;
}


/**
 * On Solaris use the old-style procfs interfaces but the core file still should have this
 * info. for backward and GDB compatibility, hence the need for this ugly function.
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param pInfo             Pointer to the old prpsinfo_t structure to update.
 */
void GetOldProcessInfo(PVBOXCORE pVBoxCore, prpsinfo_t *pInfo)
{
    AssertReturnVoid(pVBoxCore);
    AssertReturnVoid(pInfo);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    psinfo_t *pSrc = &pVBoxProc->ProcInfo;
    memset(pInfo, 0, sizeof(prpsinfo_t));
    pInfo->pr_state    = pSrc->pr_lwp.pr_state;
    pInfo->pr_zomb     = (pInfo->pr_state == SZOMB);
    RTStrCopy(pInfo->pr_clname, sizeof(pInfo->pr_clname), pSrc->pr_lwp.pr_clname);
    RTStrCopy(pInfo->pr_fname, sizeof(pInfo->pr_fname), pSrc->pr_fname);
    memcpy(&pInfo->pr_psargs, &pSrc->pr_psargs, sizeof(pInfo->pr_psargs));
    pInfo->pr_nice     = pSrc->pr_lwp.pr_nice;
    pInfo->pr_flag     = pSrc->pr_lwp.pr_flag;
    pInfo->pr_uid      = pSrc->pr_uid;
    pInfo->pr_gid      = pSrc->pr_gid;
    pInfo->pr_pid      = pSrc->pr_pid;
    pInfo->pr_ppid     = pSrc->pr_ppid;
    pInfo->pr_pgrp     = pSrc->pr_pgid;
    pInfo->pr_sid      = pSrc->pr_sid;
    pInfo->pr_addr     = (caddr_t)pSrc->pr_addr;
    pInfo->pr_size     = pSrc->pr_size;
    pInfo->pr_rssize   = pSrc->pr_rssize;
    pInfo->pr_wchan    = (caddr_t)pSrc->pr_lwp.pr_wchan;
    pInfo->pr_start    = pSrc->pr_start;
    pInfo->pr_time     = pSrc->pr_time;
    pInfo->pr_pri      = pSrc->pr_lwp.pr_pri;
    pInfo->pr_oldpri   = pSrc->pr_lwp.pr_oldpri;
    pInfo->pr_cpu      = pSrc->pr_lwp.pr_cpu;
    pInfo->pr_ottydev  = cmpdev(pSrc->pr_ttydev);
    pInfo->pr_lttydev  = pSrc->pr_ttydev;
    pInfo->pr_syscall  = pSrc->pr_lwp.pr_syscall;
    pInfo->pr_ctime    = pSrc->pr_ctime;
    pInfo->pr_bysize   = pSrc->pr_size * PAGESIZE;
    pInfo->pr_byrssize = pSrc->pr_rssize * PAGESIZE;
    pInfo->pr_argc     = pSrc->pr_argc;
    pInfo->pr_argv     = (char **)pSrc->pr_argv;
    pInfo->pr_envp     = (char **)pSrc->pr_envp;
    pInfo->pr_wstat    = pSrc->pr_wstat;
    pInfo->pr_pctcpu   = pSrc->pr_pctcpu;
    pInfo->pr_pctmem   = pSrc->pr_pctmem;
    pInfo->pr_euid     = pSrc->pr_euid;
    pInfo->pr_egid     = pSrc->pr_egid;
    pInfo->pr_aslwpid  = 0;
    pInfo->pr_dmodel   = pSrc->pr_dmodel;
}


/**
 * On Solaris use the old-style procfs interfaces but the core file still should have this
 * info. for backward and GDB compatibility, hence the need for this ugly function.
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param pInfo             Pointer to the thread info.
 * @param pStatus           Pointer to the thread status.
 * @param pDst              Pointer to the old-style status structure to update.
 *
 */
void GetOldProcessStatus(PVBOXCORE pVBoxCore, lwpsinfo_t *pInfo, lwpstatus_t *pStatus, prstatus_t *pDst)
{
    AssertReturnVoid(pVBoxCore);
    AssertReturnVoid(pInfo);
    AssertReturnVoid(pStatus);
    AssertReturnVoid(pDst);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    memset(pDst, 0, sizeof(prstatus_t));
    if (pStatus->pr_flags & PR_STOPPED)
        pDst->pr_flags = 0x0001;
    if (pStatus->pr_flags & PR_ISTOP)
        pDst->pr_flags = 0x0002;
    if (pStatus->pr_flags & PR_DSTOP)
        pDst->pr_flags = 0x0004;
    if (pStatus->pr_flags & PR_ASLEEP)
        pDst->pr_flags = 0x0008;
    if (pStatus->pr_flags & PR_FORK)
        pDst->pr_flags = 0x0010;
    if (pStatus->pr_flags & PR_RLC)
        pDst->pr_flags = 0x0020;
    /* PR_PTRACE is never set */
    if (pStatus->pr_flags & PR_PCINVAL)
        pDst->pr_flags = 0x0080;
    if (pStatus->pr_flags & PR_ISSYS)
        pDst->pr_flags = 0x0100;
    if (pStatus->pr_flags & PR_STEP)
        pDst->pr_flags = 0x0200;
    if (pStatus->pr_flags & PR_KLC)
        pDst->pr_flags = 0x0400;
    if (pStatus->pr_flags & PR_ASYNC)
        pDst->pr_flags = 0x0800;
    if (pStatus->pr_flags & PR_PTRACE)
        pDst->pr_flags = 0x1000;
    if (pStatus->pr_flags & PR_MSACCT)
        pDst->pr_flags = 0x2000;
    if (pStatus->pr_flags & PR_BPTADJ)
        pDst->pr_flags = 0x4000;
    if (pStatus->pr_flags & PR_ASLWP)
        pDst->pr_flags = 0x8000;

    pDst->pr_who        = pStatus->pr_lwpid;
    pDst->pr_why        = pStatus->pr_why;
    pDst->pr_what       = pStatus->pr_what;
    pDst->pr_info       = pStatus->pr_info;
    pDst->pr_cursig     = pStatus->pr_cursig;
    pDst->pr_sighold    = pStatus->pr_lwphold;
    pDst->pr_altstack   = pStatus->pr_altstack;
    pDst->pr_action     = pStatus->pr_action;
    pDst->pr_syscall    = pStatus->pr_syscall;
    pDst->pr_nsysarg    = pStatus->pr_nsysarg;
    pDst->pr_lwppend    = pStatus->pr_lwppend;
    pDst->pr_oldcontext = (ucontext_t *)pStatus->pr_oldcontext;
    memcpy(pDst->pr_reg, pStatus->pr_reg, sizeof(pDst->pr_reg));
    memcpy(pDst->pr_sysarg, pStatus->pr_sysarg, sizeof(pDst->pr_sysarg));
    RTStrCopy(pDst->pr_clname, sizeof(pDst->pr_clname), pStatus->pr_clname);

    pDst->pr_nlwp       = pVBoxProc->ProcStatus.pr_nlwp;
    pDst->pr_sigpend    = pVBoxProc->ProcStatus.pr_sigpend;
    pDst->pr_pid        = pVBoxProc->ProcStatus.pr_pid;
    pDst->pr_ppid       = pVBoxProc->ProcStatus.pr_ppid;
    pDst->pr_pgrp       = pVBoxProc->ProcStatus.pr_pgid;
    pDst->pr_sid        = pVBoxProc->ProcStatus.pr_sid;
    pDst->pr_utime      = pVBoxProc->ProcStatus.pr_utime;
    pDst->pr_stime      = pVBoxProc->ProcStatus.pr_stime;
    pDst->pr_cutime     = pVBoxProc->ProcStatus.pr_cutime;
    pDst->pr_cstime     = pVBoxProc->ProcStatus.pr_cstime;
    pDst->pr_brkbase    = (caddr_t)pVBoxProc->ProcStatus.pr_brkbase;
    pDst->pr_brksize    = pVBoxProc->ProcStatus.pr_brksize;
    pDst->pr_stkbase    = (caddr_t)pVBoxProc->ProcStatus.pr_stkbase;
    pDst->pr_stksize    = pVBoxProc->ProcStatus.pr_stksize;

    pDst->pr_processor  = (short)pInfo->pr_onpro;
    pDst->pr_bind       = (short)pInfo->pr_bindpro;
    pDst->pr_instr      = pStatus->pr_instr;
}


/**
 * Count the number of sections which will be dumped into the core file.
 *
 * @param pVBoxCore             Pointer to the core object.
 *
 * @return Number of sections for the core file.
 */
uint32_t CountSections(PVBOXCORE pVBoxCore)
{
    /* @todo sections */
    NOREF(pVBoxCore);
    return 0;
}


/**
 * Resume all threads of this process.
 *
 * @param pVBoxProc         Pointer to the VBox process.
 *
 * @return VBox error code.
 */
int ResumeAllThreads(PVBOXPROCESS pVBoxProc)
{
    AssertReturn(pVBoxProc, VERR_INVALID_POINTER);

    char szCurThread[128];
    char szPath[PATH_MAX];
    PRTDIR pDir = NULL;

    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/lwp", (int)pVBoxProc->Process);
    RTStrPrintf(szCurThread, sizeof(szCurThread), "%d", (int)pVBoxProc->hCurThread);

    int32_t cRunningThreads = 0;
    int rc = RTDirOpen(&pDir, szPath);
    if (RT_SUCCESS(rc))
    {
        /*
         * Loop through all our threads & resume them.
         */
        RTDIRENTRY DirEntry;
        while (RT_SUCCESS(RTDirRead(pDir, &DirEntry, NULL)))
        {
            if (   !strcmp(DirEntry.szName, ".")
                || !strcmp(DirEntry.szName, ".."))
                continue;

            if ( !strcmp(DirEntry.szName, szCurThread))
                continue;

            int32_t ThreadId = RTStrToInt32(DirEntry.szName);
            _lwp_continue((lwpid_t)ThreadId);
            ++cRunningThreads;
        }

        CORELOG(("ResumeAllThreads: resumed %d threads\n", cRunningThreads));
        RTDirClose(pDir);
    }
    else
    {
        CORELOGREL(("ResumeAllThreads: Failed to open %s\n", szPath));
        rc = VERR_READ_ERROR;
    }

    return rc;
}


/**
 * Stop all running threads of this process. Before dumping any
 * core we need to make sure the process is quiesced.
 *
 * @param pVBoxProc         Pointer to the VBox process.
 *
 * @return VBox error code.
 */
int SuspendAllThreads(PVBOXPROCESS pVBoxProc)
{
    char szCurThread[128];
    char szPath[PATH_MAX];
    PRTDIR pDir = NULL;

    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/lwp", (int)pVBoxProc->Process);
    RTStrPrintf(szCurThread, sizeof(szCurThread), "%d", (int)pVBoxProc->hCurThread);

    int rc = -1;
    uint32_t cThreads = 0;
    uint16_t cTries = 0;
    for (cTries = 0; cTries < 10; cTries++)
    {
        uint32_t cRunningThreads = 0;
        rc = RTDirOpen(&pDir, szPath);
        if (RT_SUCCESS(rc))
        {
            /*
             * Loop through all our threads & suspend them, multiple calls to _lwp_suspend() are okay.
             */
            RTDIRENTRY DirEntry;
            while (RT_SUCCESS(RTDirRead(pDir, &DirEntry, NULL)))
            {
                if (   !strcmp(DirEntry.szName, ".")
                    || !strcmp(DirEntry.szName, ".."))
                    continue;

                if ( !strcmp(DirEntry.szName, szCurThread))
                    continue;

                int32_t ThreadId = RTStrToInt32(DirEntry.szName);
                _lwp_suspend((lwpid_t)ThreadId);
                ++cRunningThreads;
            }

            if (cTries > 5 && cThreads == cRunningThreads)
            {
                rc = VINF_SUCCESS;
                break;
            }
            cThreads = cRunningThreads;
            RTDirClose(pDir);
        }
        else
        {
            CORELOGREL(("SuspendAllThreads: Failed to open %s cTries=%d\n", szPath, cTries));
            rc = VERR_READ_ERROR;
            break;
        }
    }

    if (RT_SUCCESS(rc))
        CORELOG(("Stopped %u threads successfully with %u tries\n", cThreads, cTries));

    return rc;
}


/**
 * Returns size of an ELF NOTE header given the size of data the NOTE section will contain.
 *
 * @param cb                Size of the data.
 *
 * @return Size of data actually used for NOTE header and section.
 */
inline size_t ElfNoteHeaderSize(size_t cb)
{
    return sizeof(ELFNOTEHDR) + RT_ALIGN_Z(cb, 4);
}


/**
 * Write an ELF NOTE header into the core file.
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param Type              Type of this NOTE section.
 * @param pcv               Opaque pointer to the data, if NULL only computes size.
 * @param cb                Size of the data.
 *
 * @return VBox status code.
 */
int ElfWriteNoteHeader(PVBOXCORE pVBoxCore, uint_t Type, const void *pcv, size_t cb)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);
    AssertReturn(pcv, VERR_INVALID_POINTER);
    AssertReturn(cb > 0, VERR_NO_DATA);
    AssertReturn(pVBoxCore->pfnWriter, VERR_WRITE_ERROR);
    AssertReturn(pVBoxCore->hCoreFile, VERR_INVALID_HANDLE);

    int rc = VERR_GENERAL_FAILURE;
#ifdef RT_OS_SOLARIS
    ELFNOTEHDR ElfNoteHdr;
    RT_ZERO(ElfNoteHdr);
    ElfNoteHdr.achName[0] = 'C';
    ElfNoteHdr.achName[1] = 'O';
    ElfNoteHdr.achName[2] = 'R';
    ElfNoteHdr.achName[3] = 'E';
    ElfNoteHdr.Hdr.n_namesz = 5;
    ElfNoteHdr.Hdr.n_type = Type;
    ElfNoteHdr.Hdr.n_descsz = RT_ALIGN_Z(cb, 4);

    /*
     * Write note header and description.
     */
    rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &ElfNoteHdr, sizeof(ElfNoteHdr));
    if (RT_SUCCESS(rc))
       rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, pcv, ElfNoteHdr.Hdr.n_descsz);

    if (RT_FAILURE(rc))
        CORELOGREL(("ElfWriteNote: pfnWriter failed. Type=%d rc=%Rrc\n", Type, rc));
#else
#error Port Me!
#endif
    return rc;
}


/**
 * Computes the size of NOTE section for the given core type.
 * Solaris has two types of program header information (new and old).
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param enmType           Type of core file information required.
 *
 * @return Size of NOTE section.
 */
size_t ElfNoteSectionSize(PVBOXCORE pVBoxCore, VBOXSOLCORETYPE enmType)
{
    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    size_t cb = 0;
    switch (enmType)
    {
        case enmOldEra:
        {
            cb += ElfNoteHeaderSize(sizeof(prpsinfo_t));
            cb += ElfNoteHeaderSize(pVBoxProc->cAuxVecs * sizeof(auxv_t));
            cb += ElfNoteHeaderSize(strlen(pVBoxProc->szPlatform));

            PVBOXSOLTHREADINFO pThreadInfo = pVBoxProc->pThreadInfoHead;
            while (pThreadInfo)
            {
                if (pThreadInfo->pStatus)
                {
                    cb += ElfNoteHeaderSize(sizeof(prstatus_t));
                    cb += ElfNoteHeaderSize(sizeof(prfpregset_t));
                }
                pThreadInfo = pThreadInfo->pNext;
            }

            break;
        }

        case enmNewEra:
        {
            cb += ElfNoteHeaderSize(sizeof(psinfo_t));
            cb += ElfNoteHeaderSize(sizeof(pstatus_t));
            cb += ElfNoteHeaderSize(pVBoxProc->cAuxVecs * sizeof(auxv_t));
            cb += ElfNoteHeaderSize(strlen(pVBoxProc->szPlatform) + 1);
            cb += ElfNoteHeaderSize(sizeof(struct utsname));
            cb += ElfNoteHeaderSize(sizeof(core_content_t));
            cb += ElfNoteHeaderSize(pVBoxProc->cbCred);

            if (pVBoxProc->pPriv)
                cb += ElfNoteHeaderSize(PRIV_PRPRIV_SIZE(pVBoxProc->pPriv));   /* Ought to be same as cbPriv!? */

            if (pVBoxProc->pcPrivImpl)
                cb += ElfNoteHeaderSize(PRIV_IMPL_INFO_SIZE(pVBoxProc->pcPrivImpl));

            cb += ElfNoteHeaderSize(strlen(pVBoxProc->szZoneName) + 1);
            if (pVBoxProc->cbLdt > 0)
                cb += ElfNoteHeaderSize(pVBoxProc->cbLdt);

            PVBOXSOLTHREADINFO pThreadInfo = pVBoxProc->pThreadInfoHead;
            while (pThreadInfo)
            {
                cb += ElfNoteHeaderSize(sizeof(lwpsinfo_t));
                if (pThreadInfo->pStatus)
                    cb += ElfNoteHeaderSize(sizeof(lwpstatus_t));

                pThreadInfo = pThreadInfo->pNext;
            }

            break;
        }

        default:
        {
            CORELOGREL(("ElfNoteSectionSize: Unknown segment era %d\n", enmType));
            break;
        }
    }

    return cb;
}


/**
 * Write the note section for the given era into the core file.
 * Solaris has two types of program  header information (new and old).
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param enmType           Type of core file information required.
 *
 * @return VBox status code.
 */
int ElfWriteNoteSection(PVBOXCORE pVBoxCore, VBOXSOLCORETYPE enmType)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    int rc = VERR_GENERAL_FAILURE;

#ifdef RT_OS_SOLARIS

    typedef int (*PFNELFWRITENOTEHDR)(PVBOXCORE pVBoxCore, uint_t, const void *pcv, size_t cb);
    typedef struct ELFWRITENOTE
    {
        const char        *pszType;
        uint_t             Type;
        const void        *pcv;
        size_t             cb;
    } ELFWRITENOTE;

    switch (enmType)
    {
        case enmOldEra:
        {
            ELFWRITENOTE aElfNotes[] =
            {
                { "NT_PRPSINFO", NT_PRPSINFO, &pVBoxProc->ProcInfoOld,  sizeof(prpsinfo_t) },
                { "NT_AUXV",     NT_AUXV,      pVBoxProc->pAuxVecs,      pVBoxProc->cAuxVecs * sizeof(auxv_t) },
                { "NT_PLATFORM", NT_PLATFORM,  pVBoxProc->szPlatform,    strlen(pVBoxProc->szPlatform) + 1 }
            };

            for (unsigned i = 0; i < RT_ELEMENTS(aElfNotes); i++)
            {
                rc = ElfWriteNoteHeader(pVBoxCore, aElfNotes[i].Type, aElfNotes[i].pcv, aElfNotes[i].cb);
                if (RT_FAILURE(rc))
                {
                    CORELOGREL(("ElfWriteNoteSection: ElfWriteNoteHeader failed for %s. rc=%Rrc\n", aElfNotes[i].pszType, rc));
                    break;
                }
            }

            /*
             * Write old-style thread info., they contain nothing about zombies,
             * so we just skip if there is no status information for them.
             */
            PVBOXSOLTHREADINFO pThreadInfo = pVBoxProc->pThreadInfoHead;
            for (; pThreadInfo; pThreadInfo = pThreadInfo->pNext)
            {
                if (!pThreadInfo->pStatus)
                    continue;

                prstatus_t OldProcessStatus;
                GetOldProcessStatus(pVBoxCore, &pThreadInfo->Info, pThreadInfo->pStatus, &OldProcessStatus);
                rc = ElfWriteNoteHeader(pVBoxCore, NT_PRSTATUS, &OldProcessStatus, sizeof(prstatus_t));
                if (RT_SUCCESS(rc))
                {
                    rc = ElfWriteNoteHeader(pVBoxCore, NT_PRFPREG, &pThreadInfo->pStatus->pr_fpreg, sizeof(prfpregset_t));
                    if (RT_FAILURE(rc))
                    {
                        CORELOGREL(("ElfWriteSegment: ElfWriteNote failed for NT_PRFPREF. rc=%Rrc\n", rc));
                        break;
                    }
                }
                else
                {
                    CORELOGREL(("ElfWriteSegment: ElfWriteNote failed for NT_PRSTATUS. rc=%Rrc\n", rc));
                    break;
                }
            }
            break;
        }

        case enmNewEra:
        {
            ELFWRITENOTE aElfNotes[] =
            {
                { "NT_PSINFO",     NT_PSINFO,     &pVBoxProc->ProcInfo,     sizeof(psinfo_t) },
                { "NT_PSTATUS",    NT_PSTATUS,    &pVBoxProc->ProcStatus,   sizeof(pstatus_t) },
                { "NT_AUXV",       NT_AUXV,        pVBoxProc->pAuxVecs,     pVBoxProc->cAuxVecs * sizeof(auxv_t) },
                { "NT_PLATFORM",   NT_PLATFORM,    pVBoxProc->szPlatform,   strlen(pVBoxProc->szPlatform) + 1 },
                { "NT_UTSNAME",    NT_UTSNAME,    &pVBoxProc->UtsName,      sizeof(struct utsname) },
                { "NT_CONTENT",    NT_CONTENT,    &pVBoxProc->CoreContent,  sizeof(core_content_t) },
                { "NT_PRCRED",     NT_PRCRED,      pVBoxProc->pvCred,       pVBoxProc->cbCred },
                { "NT_PRPRIV",     NT_PRPRIV,      pVBoxProc->pPriv,        PRIV_PRPRIV_SIZE(pVBoxProc->pPriv) },
                { "NT_PRPRIVINFO", NT_PRPRIVINFO,  pVBoxProc->pcPrivImpl,   PRIV_IMPL_INFO_SIZE(pVBoxProc->pcPrivImpl) },
                { "NT_ZONENAME",   NT_ZONENAME,    pVBoxProc->szZoneName,   strlen(pVBoxProc->szZoneName) + 1 }
            };

            for (unsigned i = 0; i < RT_ELEMENTS(aElfNotes); i++)
            {
                rc = ElfWriteNoteHeader(pVBoxCore, aElfNotes[i].Type, aElfNotes[i].pcv, aElfNotes[i].cb);
                if (RT_FAILURE(rc))
                {
                    CORELOGREL(("ElfWriteNoteSection: ElfWriteNoteHeader failed for %s. rc=%Rrc\n", aElfNotes[i].pszType, rc));
                    break;
                }
            }

            /*
             * Write new-style thread info., missing lwpstatus_t indicates it's a zombie thread
             * we only dump the lwpsinfo_t in that case.
             */
            PVBOXSOLTHREADINFO pThreadInfo = pVBoxProc->pThreadInfoHead;
            for (; pThreadInfo; pThreadInfo = pThreadInfo->pNext)
            {
                rc = ElfWriteNoteHeader(pVBoxCore, NT_LWPSINFO, &pThreadInfo->Info, sizeof(lwpsinfo_t));
                if (RT_FAILURE(rc))
                {
                    CORELOGREL(("ElfWriteNoteSection: ElfWriteNoteHeader for NT_LWPSINFO failed. rc=%Rrc\n", rc));
                    break;
                }

                if (pThreadInfo->pStatus)
                {
                    rc = ElfWriteNoteHeader(pVBoxCore, NT_LWPSTATUS, pThreadInfo->pStatus, sizeof(lwpstatus_t));
                    if (RT_FAILURE(rc))
                    {
                        CORELOGREL(("ElfWriteNoteSection: ElfWriteNoteHeader for NT_LWPSTATUS failed. rc=%Rrc\n", rc));
                        break;
                    }
                }
            }
            break;
        }

        default:
        {
            CORELOGREL(("ElfWriteNoteSection: Invalid type %d\n", enmType));
            rc = VERR_GENERAL_FAILURE;
            break;
        }
    }
#else
# error Port Me!
#endif
    return rc;
}


/**
 * Write mappings into the core file.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int ElfWriteMappings(PVBOXCORE pVBoxCore)
{
    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;

    int rc = VERR_GENERAL_FAILURE;
    PVBOXSOLMAPINFO pMapInfo = pVBoxProc->pMapInfoHead;
    while (pMapInfo)
    {
        if (!pMapInfo->fError)
        {
            uint64_t k = 0;
            char achBuf[PAGE_SIZE];
            while (k < pMapInfo->pMap.pr_size)
            {
                size_t cb = RT_MIN(sizeof(achBuf), pMapInfo->pMap.pr_size - k);
                int rc2 = ReadProcAddrSpace(pVBoxProc, pMapInfo->pMap.pr_vaddr + k, &achBuf, cb);
                if (RT_FAILURE(rc2))
                {
                    CORELOGREL(("ElfWriteMappings: Failed to read mapping, can't recover. Bye. rc=%Rrc\n", rc));
                    return VERR_INVALID_STATE;
                }

                rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, achBuf, sizeof(achBuf));
                if (RT_FAILURE(rc))
                {
                    CORELOGREL(("ElfWriteMappings: pfnWriter failed. rc=%Rrc\n", rc));
                    return rc;
                }
                k += cb;
            }
        }
        else
        {
            char achBuf[RT_ALIGN_Z(sizeof(int), 8)];
            RT_ZERO(achBuf);
            memcpy(achBuf, &pMapInfo->fError, sizeof(pMapInfo->fError));
            if (sizeof(achBuf) != pMapInfo->pMap.pr_size)
                CORELOGREL(("ElfWriteMappings: Huh!? something is wrong!\n"));
            rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &achBuf, sizeof(achBuf));
            if (RT_FAILURE(rc))
            {
                CORELOGREL(("ElfWriteMappings: pfnWriter(2) failed. rc=%Rrc\n", rc));
                return rc;
            }
        }

        pMapInfo = pMapInfo->pNext;
    }

    return VINF_SUCCESS;
}

/**
 * Write program headers for all mappings into the core file.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int ElfWriteMappingHeaders(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    Phdr ProgHdr;
    RT_ZERO(ProgHdr);
    ProgHdr.p_type = PT_LOAD;

    int rc = VERR_GENERAL_FAILURE;
    PVBOXSOLMAPINFO pMapInfo = pVBoxProc->pMapInfoHead;
    while (pMapInfo)
    {
        ProgHdr.p_vaddr  = pMapInfo->pMap.pr_vaddr;     /* Virtual address of this mapping in the process address space */
        ProgHdr.p_offset = pVBoxCore->offWrite;         /* Where this mapping is located in the core file */
        ProgHdr.p_memsz  = pMapInfo->pMap.pr_size;      /* Size of the memory image of the mapping */
        ProgHdr.p_filesz = pMapInfo->pMap.pr_size;      /* Size of the file image of the mapping */

        ProgHdr.p_flags = 0;                            /* Reset fields in a loop when needed! */
        if (pMapInfo->pMap.pr_mflags & MA_READ)
            ProgHdr.p_flags |= PF_R;
        if (pMapInfo->pMap.pr_mflags & MA_WRITE)
            ProgHdr.p_flags |= PF_W;
        if (pMapInfo->pMap.pr_mflags & MA_EXEC)
            ProgHdr.p_flags |= PF_X;

        if (pMapInfo->fError)
            ProgHdr.p_flags |= PF_SUNW_FAILURE;

        rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &ProgHdr, sizeof(ProgHdr));
        if (RT_FAILURE(rc))
        {
            CORELOGREL(("ElfWriteMappingHeaders: pfnWriter failed. rc=%Rrc\n", rc));
            return rc;
        }

        pVBoxCore->offWrite += ProgHdr.p_filesz;
        pMapInfo = pMapInfo->pNext;
    }
    return rc;
}


/**
 * Write a prepared core file using a user-passed in writer function, requires all threads
 * to be in suspended state (i.e. called after CreateCore).
 *
 * @param pVBoxCore         Pointer to the core object.
 * @param pfnWriter         Pointer to the writer function to override default writer (NULL uses default).
 *
 * @remarks Resumes all suspended threads, unless it's an invalid core.
 * @return VBox status.
 */
int WriteCore(PVBOXCORE pVBoxCore, PFNCOREWRITER pfnWriter)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);

    if (!pVBoxCore->fIsValid)
        return VERR_INVALID_STATE;

    if (pfnWriter)
        pVBoxCore->pfnWriter = pfnWriter;

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    char szPath[PATH_MAX];

    /*
     * Open the process address space file.
     */
    RTStrPrintf(szPath, sizeof(szPath), "/proc/%d/as", (int)pVBoxProc->Process);
    int rc = RTFileOpen(&pVBoxProc->hAs, szPath, RTFILE_O_OPEN | RTFILE_O_READ);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: Failed to open address space, %s. rc=%Rrc\n", szPath, rc));
        goto WriteCoreDone;
    }

    /*
     * Create the core file.
     */
    RTStrPrintf(szPath, sizeof(szPath), "/export/home/ram/vbox/out/solaris.amd64/release/bin/%s", pVBoxCore->szCorePath, pVBoxCore->VBoxProc.Process); /* @todo fix this */
    rc = RTFileOpen(&pVBoxCore->hCoreFile, szPath, RTFILE_O_OPEN_CREATE | RTFILE_O_TRUNCATE | RTFILE_O_READWRITE | RTFILE_O_DENY_ALL);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: failed to open %s. rc=%Rrc\n", szPath, rc));
        goto WriteCoreDone;
    }

    pVBoxCore->offWrite = 0;
    uint32_t cProgHdrs  = pVBoxProc->cMappings + 2; /* two PT_NOTE program headers (old, new style) */
    uint32_t cSecHdrs   = CountSections(pVBoxCore);

    /*
     * Write the ELF header.
     */
    Ehdr ElfHdr;
    RT_ZERO(ElfHdr);
    ElfHdr.e_ident[EI_MAG0]  = ELFMAG0;
    ElfHdr.e_ident[EI_MAG1]  = ELFMAG1;
    ElfHdr.e_ident[EI_MAG2]  = ELFMAG2;
    ElfHdr.e_ident[EI_MAG3]  = ELFMAG3;
    ElfHdr.e_ident[EI_DATA]  = IsBigEndian() ? ELFDATA2MSB : ELFDATA2LSB;
    ElfHdr.e_type            = ET_CORE;
    ElfHdr.e_version         = EV_CURRENT;
#ifdef RT_ARCH_AMD64
    ElfHdr.e_machine         = EM_AMD64;
    ElfHdr.e_ident[EI_CLASS] = ELFCLASS64;
#else
    ElfHdr.e_machine         = EM_386;
    ElfHdr.e_ident[EI_CLASS] = ELFCLASS32;
#endif
    if (cProgHdrs >= PN_XNUM)
        ElfHdr.e_phnum       = PN_XNUM;
    else
        ElfHdr.e_phnum       = cProgHdrs;
    ElfHdr.e_ehsize          = sizeof(ElfHdr);
    ElfHdr.e_phoff           = sizeof(ElfHdr);
    ElfHdr.e_phentsize       = sizeof(Phdr);
    ElfHdr.e_shentsize       = sizeof(Shdr);
    rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &ElfHdr, sizeof(ElfHdr));
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: pfnWriter failed writing ELF header. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Setup program header.
     */
    Phdr ProgHdr;
    RT_ZERO(ProgHdr);
    ProgHdr.p_type = PT_NOTE;
    ProgHdr.p_flags = PF_R;

    /*
     * Write old-style NOTE program header.
     */
    pVBoxCore->offWrite += sizeof(ElfHdr) + cProgHdrs * sizeof(ProgHdr);
    ProgHdr.p_offset = pVBoxCore->offWrite;
    ProgHdr.p_filesz = ElfNoteSectionSize(pVBoxCore, enmOldEra);
    rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &ProgHdr, sizeof(ProgHdr));
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: pfnWriter failed writing old-style ELF program Header. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Write new-style NOTE program header.
     */
    pVBoxCore->offWrite += ProgHdr.p_filesz;
    ProgHdr.p_offset = pVBoxCore->offWrite;
    ProgHdr.p_filesz = ElfNoteSectionSize(pVBoxCore, enmNewEra);
    rc = pVBoxCore->pfnWriter(pVBoxCore->hCoreFile, &ProgHdr, sizeof(ProgHdr));
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: pfnWriter failed writing new-style ELF program header. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Write program headers per mapping.
     */
    pVBoxCore->offWrite += ProgHdr.p_filesz;
    rc = ElfWriteMappingHeaders(pVBoxCore);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("Write: ElfWriteMappings failed. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Write old-style note section.
     */
    rc = ElfWriteNoteSection(pVBoxCore, enmOldEra);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: ElfWriteNoteSection old-style failed. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Write new-style section.
     */
    rc = ElfWriteNoteSection(pVBoxCore, enmNewEra);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: ElfWriteNoteSection new-style failed. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }

    /*
     * Write all mappings.
     */
    rc = ElfWriteMappings(pVBoxCore);
    if (RT_FAILURE(rc))
    {
        CORELOGREL(("WriteCore: ElfWriteMappings failed. rc=%Rrc\n", rc));
        goto WriteCoreDone;
    }


WriteCoreDone:
    if (pVBoxCore->hCoreFile != NIL_RTFILE)
    {
        RTFileClose(pVBoxCore->hCoreFile);
        pVBoxCore->hCoreFile = NIL_RTFILE;
    }

    if (pVBoxProc->hAs != NIL_RTFILE)
    {
        RTFileClose(pVBoxProc->hAs);
        pVBoxProc->hAs = NIL_RTFILE;
    }

    ResumeAllThreads(pVBoxProc);
    return rc;
}


/**
 * Takes a process snapshot into a passed-in core object. It has the side-effect of halting
 * all threads which can lead to things like spurious wakeups of threads (if and when threads
 * are ultimately resumed en-masse) already suspended while calling this function.
 *
 * @param pVBoxCore         Pointer to a core object.
 * @param pContext          Pointer to the caller context thread.
 *
 * @remarks Halts all threads.
 * @return VBox status code.
 */
int CreateCore(PVBOXCORE pVBoxCore, ucontext_t *pContext)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);
    AssertReturn(pContext, VERR_INVALID_POINTER);

    /*
     * Initialize core structures.
     */
    memset(pVBoxCore, 0, sizeof(VBOXCORE));
    pVBoxCore->pfnReader = &ReadFileNoIntr;
    pVBoxCore->pfnWriter = &WriteFileNoIntr;
    pVBoxCore->fIsValid  = false;
    pVBoxCore->hCoreFile = NIL_RTFILE;

    PVBOXPROCESS pVBoxProc = &pVBoxCore->VBoxProc;
    pVBoxProc->Process        = RTProcSelf();
    pVBoxProc->hCurThread     = _lwp_self(); /* thr_self() */
    pVBoxProc->hAs            = NIL_RTFILE;
    pVBoxProc->pCurThreadCtx  = pContext;
    pVBoxProc->CoreContent    = CC_CONTENT_DEFAULT;

    RTProcGetExecutableName(pVBoxProc->szExecPath, sizeof(pVBoxProc->szExecPath));  /* this gets full path not just name */
    pVBoxProc->pszExecName = RTPathFilename(pVBoxProc->szExecPath);
    RTStrPrintf(pVBoxCore->szCorePath, sizeof(pVBoxCore->szCorePath), "core.vb.%s.%d", pVBoxProc->pszExecName, (int)pVBoxProc->Process);

    CORELOG(("tstRTCoreDump: Taking Core %s from Thread %d\n", pVBoxCore->szCorePath, (int)pVBoxProc->hCurThread));

    /*
     * Quiesce the process.
     */
    int rc = SuspendAllThreads(pVBoxProc);
    if (RT_SUCCESS(rc))
    {
        rc = ReadProcInfo(pVBoxCore);
        if (RT_SUCCESS(rc))
        {
            GetOldProcessInfo(pVBoxCore, &pVBoxProc->ProcInfoOld);
            if (IsProcArchNative(pVBoxProc))
            {
                /*
                 * Read process status, information such as number of active LWPs will be invalid since we just quiesced the process.
                 */
                rc = ReadProcStatus(pVBoxCore);
                if (RT_SUCCESS(rc))
                {
                    rc = AllocMemoryArea(pVBoxCore);
                    if (RT_SUCCESS(rc))
                    {
                        struct COREACCUMULATOR
                        {
                            const char        *pszName;
                            PFNCOREACCUMULATOR pfnAcc;
                            bool               fOptional;
                        } aAccumulators[] =
                        {
                            { "ReadProcLdt",      &ReadProcLdt,      false },
                            { "ReadProcCred",     &ReadProcCred,     false },
                            { "ReadProcPriv",     &ReadProcPriv,     false },
                            { "ReadProcAuxVecs",  &ReadProcAuxVecs,  false },
                            { "ReadProcMappings", &ReadProcMappings, false },
                            { "ReadProcThreads",  &ReadProcThreads,  false },
                            { "ReadProcMiscInfo", &ReadProcMiscInfo, false }
                        };

                        for (unsigned i = 0; i < RT_ELEMENTS(aAccumulators); i++)
                        {
                            rc = aAccumulators[i].pfnAcc(pVBoxCore);
                            if (RT_FAILURE(rc))
                            {
                                CORELOGREL(("DumpCore: %s failed. rc=%Rrc\n", aAccumulators[i].pszName, rc));
                                if (!aAccumulators[i].fOptional)
                                    break;
                            }
                        }

                        if (RT_SUCCESS(rc))
                        {
                            pVBoxCore->fIsValid = true;
                            return VINF_SUCCESS;
                        }

                        FreeMemoryArea(pVBoxCore);
                    }
                    else
                        CORELOGREL(("DumpCore: AllocMemoryArea failed. rc=%Rrc\n", rc));
                }
                else
                    CORELOGREL(("DumpCore: ReadProcStatus failed. rc=%Rrc\n", rc));
            }
            else
            {
                CORELOGREL(("DumpCore: IsProcArchNative failed.\n"));
                rc = VERR_BAD_EXE_FORMAT;
            }
        }
        else
            CORELOGREL(("DumpCore: ReadProcInfo failed. rc=%Rrc\n", rc));

        /*
         * Resume threads on failure.
         */
        ResumeAllThreads(pVBoxProc);
    }
    else
        CORELOG(("DumpCore: SuspendAllThreads failed. Thread bomb!?! rc=%Rrc\n", rc));

    return rc;
}


/**
 * Destroy an existing core object.
 *
 * @param pVBoxCore         Pointer to the core object.
 *
 * @return VBox status code.
 */
int DestroyCore(PVBOXCORE pVBoxCore)
{
    AssertReturn(pVBoxCore, VERR_INVALID_POINTER);
    if (!pVBoxCore->fIsValid)
        return VERR_INVALID_STATE;

    FreeMemoryArea(pVBoxCore);
    pVBoxCore->fIsValid = false;
    return VINF_SUCCESS;
}


void CoreSigHandler(int Sig, siginfo_t *pSigInfo, void *pvArg)
{
    CORELOG(("CoreSigHandler Sig=%d pvArg=%p\n", Sig, pvArg));

    ucontext_t *pContext = (ucontext_t *)pvArg;
    if (!pContext)
        CORELOGREL(("CoreSigHandler: Missing context.\n"));
    else
    {
        if (!ASMAtomicUoReadBool(&g_fCoreDumpInProgress))
        {
            ASMAtomicWriteBool(&g_fCoreDumpInProgress, true);

            /*
             * Take a snapshot, then dump core to disk, all threads except this one are halted
             * from before taking the snapshot until writing the core is completely finished.
             * Any errors would resume all threads if they were halted.
             */
            VBOXCORE VBoxCore;
            RT_ZERO(VBoxCore);
            int rc = CreateCore(&VBoxCore, pContext);
            if (RT_SUCCESS(rc))
            {
                rc = WriteCore(&VBoxCore, &WriteFileNoIntr);
                if (RT_SUCCESS(rc))
                    CORELOG(("CoreSigHandler: Successfully wrote core file to disk.\n"));
                else
                    CORELOGREL(("CoreSigHandler: WriteCore failed. rc=%Rrc\n", rc));

                DestroyCore(&VBoxCore);
            }
            else
                CORELOGREL(("CoreSigHandler: CreateCore failed. rc=%Rrc\n", rc));

            ASMAtomicWriteBool(&g_fCoreDumpInProgress, false);
        }
        else
        {
            /* @todo detect if we are awaiting for ourselves, if so don't. */
            CORELOGREL(("CoreSigHandler: Core dump already in progress! Waiting before signalling Sig=%d.\n", Sig));
            int64_t iTimeout = 10000;  /* timeout (ms) */
            while (!ASMAtomicUoReadBool(&g_fCoreDumpInProgress))
            {
                RTThreadSleep(200);
                iTimeout -= 200;
                if (iTimeout <= 0)
                    break;
            }
            if (iTimeout <= 0)
                CORELOGREL(("CoreSigHandler: Core dump seems to be stuck. Signalling new signal %d\n", Sig));
        }
    }

    signal(Sig, SIG_DFL);
    kill((int)getpid(), Sig);
}


static DECLCALLBACK(int) SleepyThread(RTTHREAD Thread, void *pvUser)
{
    NOREF(pvUser);
    sleep(10000);
    return VINF_SUCCESS;
}


int main()
{
    RTR3Init();
    CORELOG(("tstRTCoreDump: TESTING pid=%d\n", getpid()));

    /*
     * Install core dump signal handler.
     */
    struct sigaction sigAction;
    sigAction.sa_sigaction = CoreSigHandler;
    sigemptyset(&sigAction.sa_mask);
    sigAction.sa_flags = SA_RESTART | SA_SIGINFO;
    sigaction(SIGSEGV, &sigAction, NULL);
    sigaction(SIGBUS, &sigAction, NULL);
    sigaction(SIGUSR1, &sigAction, NULL);

    /*
     * Spawn a few threads.
     */
    RTTHREAD ahThreads[5];
    for (unsigned i = 0; i < RT_ELEMENTS(ahThreads); i++)
    {
        int rc = RTThreadCreate(&ahThreads[i], SleepyThread, &ahThreads[i], 0, RTTHREADTYPE_DEFAULT, RTTHREADFLAGS_WAITABLE, "TEST1");
        if (RT_FAILURE(rc))
        {
            CORELOG(("tstRTCoreDump: FAILURE(%d) - %d RTThreadCreate failed, rc=%Rrc\n", __LINE__, i, rc));
            g_cErrors++;
            ahThreads[i] = NIL_RTTHREAD;
            break;
        }
    }

    CORELOG(("Spawned %d threads\n", RT_ELEMENTS(ahThreads)));

    /*
     * Send signal to dump core.
     */
    kill(getpid(), SIGSEGV);
    g_cErrors++;

    sleep(10);

    /*
     * Summary.
     */
    if (!g_cErrors)
        CORELOG(("tstRTCoreDump: SUCCESS\n"));
    else
        CORELOG(("tstRTCoreDump: FAILURE - %d errors\n", g_cErrors));

    return !!g_cErrors;
}

