#include <GE.h>
#include <menuitems.h>
#include <pslib_exif.h>
#include "PersImageType.h"
#include <stripMemoryMgr.h>
#include <GE_StripDimensions.h>
#if PRINT_COLLATION == BIND
#include <dm_exif.h>
#include "rustler.h"
#endif
#ifdef AIO_POOL_SHARE
#include <AiOPool_share.h>
#endif
#include <string.h> //memcpy
#ifndef NDEBUG
#include <dm_print_public.h>
#include <IAppVariable.h>
#include <CAppVariableManager.h>
#include <SPISysDebugIntf.h>
#endif
#include <pslibctrl_inif.h> //Added to use FlushAllPages()
#include <PRSS.h>
#include <errmgr.h>

#ifdef BIND
extern const int GE_MEMORY_FOR_COMPRESSED_PAGES;
const int MEMORY_FOR_COMPRESSED_STRIP_DATA = GE_MEMORY_FOR_COMPRESSED_PAGES;
#else
const int MEMORY_FOR_COMPRESSED_STRIP_DATA = 1024 * 1024 * 10;  //10MB
#endif

#ifdef AIO_POOL_SHARE
  #ifdef MFP_PLATFORM
const uint8 debugDelayTimeout = 15;
  #endif
#endif

/////////////////////Large static allocation//////////////////////////////
//Global static buffer used to allocate from. MEMORY_FOR_COMPRESSED_STRIP_DATA is defined in stripMemoryMgr.h,
//which in turn uses GE_MEMORY_FOR_COMPRESSED_PAGES defined in ge_config.h
#ifdef AIO_POOL_SHARE
#pragma ghs section bss=".print_and_aio_pool"
#else
#pragma ghs section bss=".printmem"
#endif
ubyte memoryBuffer[MEMORY_FOR_COMPRESSED_STRIP_DATA+StripMemoryManager::alignAllocationsTo];
#pragma ghs section

extern ScalingModes GetCurrentResolutionScalingMode(void);
extern LowMemPipeAbilitiesEnum GetLowMemPipeAbilities();
extern OutputResolutionType GetOutputResolution();
extern OutputBPPEnum GetOutputBitsPerPixel();

///////////////////////////////////////////////////////////////////////////


/**********************************************************************************************//**
 * SECTION: StripMemoryManager public definitions
 ************************************************************************************************/

/**********************************************************************************************//**
 * This method obtains the singleton instance of the StripMemoryManager class
 *
 * @returns  pointer to instance of StripMemoryManager class
 ************************************************************************************************/
StripMemoryManager* StripMemoryManager::GetSingletonInstance()
{
    if(StripMemoryManager::singletonInstance)
    {
        return StripMemoryManager::singletonInstance;
    }
    else
    {
        StripMemoryManager::singletonInstance = new StripMemoryManager();
        assert(StripMemoryManager::singletonInstance);
        return singletonInstance;
    }
}

/**********************************************************************************************//**
 * This method frees all strip memory(allocated through this class) associated with a particular page.
 * Only the most recent/current or oldest page can be freed.
 *
 * @param pageID - The page to free
 ************************************************************************************************/
void StripMemoryManager::FreeAllStripsInPage(unsigned int jobID, unsigned int pageID)
{
    FreeStripsStream(jobID, pageID);
    
    ReturnPrintMemSem();
    
#ifdef AIO_POOL_SHARE
    ReturnAiOPool(); 
#endif
}

/**********************************************************************************************//**
 * Frees all memory currently allocated by this class
 *
 ************************************************************************************************/
void StripMemoryManager::FreeAllMemory()
{
    this->Initialize();
}

/**********************************************************************************************//**
 * Get all memory in print memory pool
 *
 ************************************************************************************************/
void* StripMemoryManager::GetAllMemory()
{
    if (!stripMemoryHead->avail)
    {
        CHECKPOINTA("scan try to get print memory pool while print own it");
        assert(0);
    }

    return this->GetSegment();
}

/**********************************************************************************************//**
 * Adjusts the total memory size available to the StripMemoryManager. This is intended for testing purposes.
 *
 * @pre The new memory size must be <= the size of the original memory block allocated for this class.
 * @param newTotalMemorySize - The new total size of memory available to the this memory manager.
 ************************************************************************************************/
void StripMemoryManager::SetTotalMemorySize(unsigned int newTotalMemorySize)
{
    if(newTotalMemorySize < MEMORY_FOR_COMPRESSED_STRIP_DATA)
    {
        CHECKPOINTA("newTotalMemorySize %d", newTotalMemorySize);
        if ((stripMemoryHead != NULL)&&(!stripMemoryHead->avail))
        {
            assert(0);
        }

        GetSegment();
        stripMemoryHead = NULL;

        this->totalMemorySize = newTotalMemorySize;
        this->Initialize();
    }
}

/**********************************************************************************************//**
 * Returns whether this is empty(contains strips or not)
 *
 * @return  true if empty. false otherwise
 ************************************************************************************************/
bool StripMemoryManager::StripMemoryManager::IsEmpty() const
{
    return stripMemoryHead->avail;
}

/**********************************************************************************************//**
 * Obtains total number of jobs being stored in StripMemoryManager
 * @returns  Number of jobs being stored
 ************************************************************************************************/
unsigned int StripMemoryManager::GetNumberOfJobs(void)
{
    unsigned int count = 0;
    int prevJobID = -1;

    Lock();

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        if(stream->jobID != prevJobID)
        {
            count++;
            prevJobID = stream->jobID;
        }

        stream = stream->next;
    }

    UnLock();

    return count;
}

/**********************************************************************************************//**
 * Obtains the number of pages being stored that belong to specified job
 * @param pageID    - The pageID to check
 * @returns  Number of pages in job. 0 if job not found.
 ************************************************************************************************/
unsigned int StripMemoryManager::GetNumberOfPagesInJob(unsigned int jobID)
{
    unsigned int pageCount = 0;

    Lock();
    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        if (stream->jobID == jobID)
        {
            pageCount++;
        }

        stream = stream->next;
    }

    UnLock();
    return pageCount;

}

/**********************************************************************************************//**
 * Obtains the number of strips that are allocated for the specified pageID
 * @param pageID    - The pageID to check
 * @returns  Number of strips in page. 0 if page not found.
 ************************************************************************************************/
unsigned int StripMemoryManager::GetNumberOfStripsInPage(unsigned int jobID, unsigned int pageID)
{

    //Loop until another page or no more pages is reached
    unsigned int numStripsInThisPage = 0;

    Lock();

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        if ((stream->jobID == jobID) && (stream->pageID == pageID))
        {
            numStripsInThisPage = stream->numStrips;
            break;
        }

        stream = stream->next;
    }

    UnLock();
    return numStripsInThisPage;
}

/**********************************************************************************************//**
 * Obtains the total number of strips being stored.
 * @returns  Total number of strips being stored.
 ************************************************************************************************/
unsigned int StripMemoryManager::GetTotalNumberOfStrips()
{
    //Loop until there are no more allocations
    unsigned int totalNumStrips = 0;

    Lock();

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        totalNumStrips += stream->numStrips;
        stream = stream->next;
    }

    UnLock();
    return totalNumStrips;
}

/**********************************************************************************************//**
 * Obtains the number of the current strip being processed.
 * @returns  number of the current strip being processed.
 ************************************************************************************************/
unsigned int StripMemoryManager::GetCurrentStripNum()
{
     
     unsigned int currStripNum = 0;

     Lock();

     StripStream* stream = stripStreamHead;

     currStripNum = stream->numStrips;

     UnLock();

     return currStripNum;

}

/**********************************************************************************************//**
 * Obtains the allocated data for specified strip. This will the same pointer originally returned by
 * GetMemoryForNextStrip() or ReplaceStrip()
 * @param pageID   - The pageID to check
 * @param stripID  - The ID of the strip to query
 * @returns  Number of strips in page. 0 if page not found.
 ************************************************************************************************/
void* StripMemoryManager::GetStripsAllocatedData(unsigned int jobID, unsigned int pageID, unsigned int stripID)
{
    Lock();

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        if ((stream->jobID == jobID) && (stream->pageID == pageID))
            break;

        stream = stream->next;
    }

    if (stream == NULL)
    {
        UnLock();
        return NULL;
    }

    if (stripID >= stream->numStrips)
    {
        UnLock();
        return NULL;
    }
    
    UnLock();
    return stream->strips[stripID];
}

/**********************************************************************************************//**
 * Obtains the size of a strip allocation
 * @param pageID    - The page this strip allocation belongs to
 * @param stripID  - The number of the strip within the page
 * @returns  size in bytes of strip allocation. 0 If strip is not found.
 ************************************************************************************************/
unsigned int StripMemoryManager::GetSizeOfStrip(unsigned int jobID, unsigned int pageID, unsigned int stripID)
{
    Lock();

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        if ((stream->jobID == jobID) && (stream->pageID == pageID))
            break;

        stream = stream->next;
    }

    if (stream == NULL)
    {
        UnLock();
        return 0;
    }

    if (stripID >= stream->numStrips)
    {
        UnLock();
        return 0;
    }

    UnLock();
    return stream->strips[stripID]->compressSize;

}

/**********************************************************************************************//**
 * Obtains the total space(used and free) in this memory manager.
 * @returns  size in bytes
 ************************************************************************************************/
unsigned int StripMemoryManager::GetTotalSpaceInBytes() const
{
    return this->totalMemorySize;
}

/**********************************************************************************************//**
 * Obtains the free space in this memory manager. Note that if the free space is >= a requested future allocation,
 * the allocation is not gauranteed to succeeed(the free space can be broken into two parts).
 *
 * @returns  size of free space in bytes
 ************************************************************************************************/
unsigned int StripMemoryManager::GetFreeSpaceInBytes()
{
    unsigned int freeSpace = 0;

    Lock();

    StripMemorySegment* p = stripMemoryHead;

    while (p != NULL)
    {
        if (p->avail)
            freeSpace += p->endAddress - p->startAddress + GetAlignedSize(sizeof(StripMemorySegment));
        else
            freeSpace += p->endAddress - p->currentAddress + GetAlignedSize(sizeof(StripMemorySegment));

        p = p->next;
    }

    StripStream* stream = stripStreamHead;

    while(stream != NULL)
    {
        freeSpace += GetAlignedSize(sizeof(StripStream));
        stream = stream->next;
    }

    UnLock();
    return freeSpace;
}


/**********************************************************************************************//**
 * SECTION: StripMemoryManager private/protected definitions
 ************************************************************************************************/


/**********************************************************************************************//**
 * Static variables
 ************************************************************************************************/
StripMemoryManager* StripMemoryManager::singletonInstance = NULL;
sem_t * StripMemoryManager::printmemSem = NULL;

/**********************************************************************************************//**
 * Constructor for StripMemoryManager class. This class cannot be instantiated externally. Clients
 * must use the GetSingletonInstance() static method.
 ************************************************************************************************/
StripMemoryManager::StripMemoryManager()
{
    this->totalMemorySize = MEMORY_FOR_COMPRESSED_STRIP_DATA;

    #ifndef UT_TEST
    sem_init(&stream_page_op_sem, 1, 1);
    sem_init(&stream_op_sem, 1, 1);
    sem_init(&stream_alloc_sem, 1, 1);
    sem_init(&free_segment_sem, 1, 0);


    pthread_mutexattr_t    mattr;
    int                    sz;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    sz = pthread_mutex_init(&mempipe_mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);
    ASSERT(sz == 0);
    
    // Here we need to initialize our datadownload semaphore.
    // LockDownloadMemories will need to call in here to get a 
    // handle on the semaphore...
    this->printmemSem = sem_open("PRINTMEM", O_CREAT, 0, 1);
    if( this->printmemSem == (sem_t*)ERROR )
    {
        CHECKPOINTA( "ERROR: Failed to initialize printmem semaphore");
        ASSERT( false );
    }
    #endif
    
    this->printmemSemTaken = false;
#ifdef AIO_POOL_SHARE
    this->AiOPoolSemTaken = false;
#endif

    this->Initialize();
}

/********************************************************************************
 * Return a handle to the printmemSem so that a caller can check it themselves
 * @author DWayda
 * @date 4/26/17
 *******************************************************************************/
sem_t * StripMemoryManager::GetPrintMemSemHandle(void)
{
    return this->printmemSem;
}

/********************************************************************************
 * Attempt to have the StripMemoryMgr take hold of the semaphore that is
 *  used in LockDownloadMemories to see if Print is still processing.
 * @author DWayda
 * @date 4/28/17
 *******************************************************************************/
void StripMemoryManager::GetPrintMemSem(void)
{    
    if (this->printmemSemTaken == false)
    {
        sem_wait(this->printmemSem);
        this->printmemSemTaken = true;
    }
}

/********************************************************************************
 * Give up ownership of the .printmem segment semaphore
 * @author DWayda
 * @date 6/15/17
 *******************************************************************************/
void StripMemoryManager::ReturnPrintMemSem(void)
{    
    // Next, we should check if there are no more pages in the StripMemoryManager
    // If it's empty, we can give back the semaphore showing dataDownload that the 
    // .printmem memory is in use.
    CHECKPOINTA("Looking if printmemSem is Taken");
    if (this->printmemSemTaken == true)
    {
        CHECKPOINTA("Semaphore is taken");
        // Check if we're empty.
        if (stripMemoryHead->avail)
        {
#ifdef AIO_POOL_SHARE
            Lock();
            GetSegment();
            stripMemoryHead = NULL;
            UnLock();
#endif
            CHECKPOINTA("Ok, store is empty so we're releasing semaphore!");
            // ok, so we're empty.
            sem_post(this->printmemSem);
            this->printmemSemTaken = false;
        }
        CHECKPOINTA("Done checking semaphore!");
    } 
}

#ifdef AIO_POOL_SHARE
/********************************************************************************
 * Attempt to have the StripMemoryMgr take hold of the AiO Pool via the owner
 *  semaphore inside of AiOPool_share.c
 * @author DWayda
 * @date 4/28/17
 *******************************************************************************/
void StripMemoryManager::GetAiOPoolOwnership(void)
{    
#ifdef MFP_PLATFORM
    if (this->AiOPoolSemTaken == false)
    {
        uint32 tries = 0;
        while(!tryAiOPoolSemTake(eAiOPoolOwnerEnum_Print))
        {
            tries++;
            tickToSleep( 1000 );  // Sleep for a second to allow scanner to finish.
            if ((tries % 60) == debugDelayTimeout)
            {
                CHECKPOINTA("Unable to get AiOPool, on try %d", tries);
            }
        }
        this->AiOPoolSemTaken = true;
        Lock();
        InitializeSegments();
        UnLock();
    }
#endif
}

/********************************************************************************
 * Give up ownership of the AiOPool.
 * @author DWayda
 * @date 4/28/17
 *******************************************************************************/
void StripMemoryManager::ReturnAiOPool(void)
{
#ifdef MFP_PLATFORM
    if (this->AiOPoolSemTaken == true)
    {
        if (stripMemoryHead->avail)
        {
            Lock();
            GetSegment();
            stripMemoryHead = NULL;
            UnLock();
            AiOPoolSemReturn(eAiOPoolOwnerEnum_Print);
            this->AiOPoolSemTaken = false;
        }
    }
#endif
}
#endif

/**********************************************************************************************//**
 * Initializes all member variables to default initial values. This will effective reset all allocations
 * to free.
 ************************************************************************************************/
void StripMemoryManager::Initialize()
{
#ifndef AIO_POOL_SHARE
    // Initialize our main segment.
    Lock();
    InitializeSegments();
    UnLock();
#endif
}


void StripMemoryManager::FreeSegments(StripMemorySegment* segment, int num)
{
    int final_segments_num = num;

    if (segment != stripMemoryHead)
        assert(0);
    //remove segment in linklist

    for (int i = 0; i < num; i++)
    {
        stripMemoryHead->currentAddress = stripMemoryHead->startAddress;
        stripMemoryHead->avail = true;

        if (i == num -1)
        {
            StripMemorySegment* p = stripMemoryHead->next;
            stripMemoryHead->next = NULL;
            stripMemoryHead = p;
        }
        else
            stripMemoryHead = stripMemoryHead->next;
    }


    //add segment in tail
    StripMemorySegment* p = stripMemoryHead;
    if (p == NULL)
    {
        stripMemoryHead = segment;
        final_segments_num -= CombineSegments(segment, num);
    }
    else
    {
        while (p->next != NULL)
            p = p->next;

        p->next = segment;

        if (p->avail)
            final_segments_num -= CombineSegments(p, num+1);
        else
            final_segments_num -= CombineSegments(segment, num);
    }
    CHECKPOINTB("final_segments_num %d", final_segments_num);
    #ifndef UT_TEST
    for (int i = 0; i < final_segments_num; i++)
    {
        CHECKPOINTB("posting the free_segment_sem");
        sem_post(&free_segment_sem);
    }
    #endif

}
bool StripMemoryManager::JobIsCanceled(unsigned int jobID){
    StripStream * stream = GetSingletonInstance()->stripStreamHead;

    if (stream->cancelLock)
        return false;

    bool retValue = false;
    while(stream != NULL)
    {
        if(stream->jobID == jobID)
            break;
        stream = stream->next;
    }
    if(stream == NULL)
    {
       CHECKPOINTA("Didn't find the jobID");
       return retValue;//when not matching any stream,job imply canceled
    }
    else
    {
      retValue = stream->jobCanceledbit;

    }
    CHECKPOINTA("jobId %u has been setted to %d",jobID,retValue);
    return retValue;
}

void StripMemoryManager::SetJobStatus(unsigned int jobID,bool isCanceled){
    StripStream * stream = GetSingletonInstance()->stripStreamHead;
    int jobStreamCounter = 0;
    while(stream != NULL)
    {
        if(stream->jobID == jobID)
        {
            stream->jobCanceledbit = isCanceled;
            jobStreamCounter++;
        }
        stream = stream->next;
    }
    CHECKPOINTA("Changed %d stream's status to %d",jobStreamCounter,isCanceled);
}

void StripMemoryManager::NewSegment(StripMemorySegment* head, unsigned int startAddress, unsigned int endAddress)
{
    StripMemorySegment* segment = (StripMemorySegment*)startAddress;
    segment->startAddress = startAddress + GetAlignedSize(sizeof(StripMemorySegment));
    segment->endAddress = endAddress;
    segment->currentAddress = segment->startAddress;
    segment->avail = true;
    segment->next = NULL;


    if (head == NULL)
    {
        stripMemoryHead = segment;
    }
    else
    {
        segment->next = head->next;
        head->next = segment;

    }

    #ifndef UT_TEST
    CHECKPOINTB("posting the free_segment_sem"); 
    sem_post(&free_segment_sem);
    #endif
}

int StripMemoryManager::CombineSegments(StripMemorySegment* head, int steps)
{
    int merge_count = 0;
    StripMemorySegment* p = head;
    StripMemorySegment* q;

    for (int i = 0; i < steps -1; i++)
    {
        q = p->next;
        if (p->startAddress < q->startAddress)
        {
            p->endAddress = q->endAddress;
            p->next = q->next;
            merge_count ++;
        }
        else
            p = q;

    }

    return merge_count;
}

void StripMemoryManager::InitializeSegments(void)
{
    CHECKPOINTA("Setting currentStripStream as Null");
    stripMemoryHead = NULL;
    stripStreamHead = NULL;
    currentStripStream = NULL;

    uint32 start = GetStripMemoryStartAddress();
    NewSegment(NULL, start, start + this->totalMemorySize);
}

StripMemorySegment* StripMemoryManager::GetSegment(void)
{
    struct timespec waittime;
    int result = 0;
    waittime.tv_nsec = 250000000; //250ms
    waittime.tv_sec  = 0;
 
    if ((stripMemoryHead->avail)&&(stripMemoryHead->next != NULL))
    {
        DumpStripStreams();
        StripMemorySegment* p = stripMemoryHead;

        while ((p != NULL) && (p->avail))
        {
            CHECKPOINTB("sem_wait p->avail");
            #ifndef UT_TEST
            CHECKPOINTB("Waiting on free_segment_sem");
            result = sem_timedwait(&free_segment_sem, &waittime);
            if (result == ERROR)
            {
                CHECKPOINTB("free_segment_sem timedout, no free segments available at the moment");
                // Release any locks to let any page returns by dps post the free_segment_sem
                UnLock(); 
                sem_wait(&free_segment_sem);
                Lock(); 
            }
            CHECKPOINTB("got free_segment_sem");
            #endif
            p->avail = false;
            p = p->next;
        }
        
        InitializeSegments();
    }

    #ifndef UT_TEST
    CHECKPOINTB("Waiting on free_segment_sem");
    waittime.tv_nsec = 250000000; //250ms
    waittime.tv_sec  = 0;
    result = sem_timedwait(&free_segment_sem, &waittime);
    if (result == ERROR)
    {
        CHECKPOINTB("free_segment_sem timedout, no free segments available at the moment");
        // Release any locks to let any page returns by dps post the free_segment_sem
        UnLock(); 
        sem_wait(&free_segment_sem);
        Lock(); 
    }
    CHECKPOINTB("got free_segment_sem");
    #endif

    StripMemorySegment* p = stripMemoryHead;

    while ((p != NULL) && (!p->avail))
        p = p->next;

    if (p == NULL)
    {
        assert(0);
    }

    p->avail = false;
    p->currentAddress = p->startAddress;

    return p;
}

StripMemorySegment* StripMemoryManager::GetNextSegment(StripStream* stream)
{
    DumpStripStreams();
    struct timespec waittime;
    int result = 0;
    waittime.tv_nsec = 250000000; //250ms
    waittime.tv_sec  = 0;
 
    #ifndef UT_TEST
    StripMemorySegment *segment = NULL;
    bool jobCanceledbit = JobIsCanceled(stream->jobID);
    if(jobCanceledbit){
       CHECKPOINTA("job is canceled!job id:%u",stream->jobID);
       return segment;
    }
    else{
       segment = stream->currentSegment;
    }
    CHECKPOINTB("Waiting on free_segment_sem");
    result = sem_timedwait(&free_segment_sem, &waittime);
    if (result == ERROR)
    {
        CHECKPOINTB("free_segment_sem timedout, no free segments available at the moment");
        // Release any locks to let any page returns by dps post the free_segment_sem
        UnLock(); 
        while(sem_timedwait(&free_segment_sem,&waittime) == ERROR)
        {
           if( !JobIsCanceled(stream->jobID)) 
               CHECKPOINTA("free_segment_sem timedout, no free segments available at the moment and job is not canceled");
           else
           {
                CHECKPOINTA("job is canceled!job id:%u",stream->jobID);
                #if PRINT_COLLATION == BIND
                if (stripStreamHead->CollationValue > 1)
                {
                    CHECKPOINTA("collation job, DPS will not release page until end of job, return NULL to abort job");
                    ERRMGR_setErrorCode(emWARNING, MEMMGR, COLLATION_CANCEL);
                    FR_cancelCollation();
                }
                #endif
                return NULL;
           }
        }
        Lock(); 
    }
    CHECKPOINTB("got free_segment_sem");
    #endif

    CHECKPOINTB("segment->next->startAddress: %d", segment->next->startAddress);
    
    segment->next->avail = false;
    segment->next->currentAddress = segment->next->startAddress;
    CHECKPOINTB("segment->next: %d", segment->next);
    
    return segment->next;
}


void StripMemoryManager::SplitSegment(StripMemorySegment* segment)
{
    CHECKPOINTB("segment->currentAddress %x", segment->currentAddress);
    CHECKPOINTB("segment->startAddress %x", segment->startAddress);
    if (segment->currentAddress - segment->startAddress < SEGMENT_MIN_SIZE)
        segment->currentAddress = segment->startAddress + SEGMENT_MIN_SIZE;

    if (segment->endAddress - segment->currentAddress > SEGMENT_MIN_SIZE)
    {
        CHECKPOINTB("segment->endAddress %x", segment->endAddress);
        CHECKPOINTB("segment->currentAddress %x", segment->currentAddress);
        unsigned int start = segment->currentAddress;
        unsigned int end = segment->endAddress;
        segment->endAddress = segment->currentAddress;
        NewSegment(segment, start, end);
    }
}

bool StripMemoryManager::IsStripMemoryAvail()
{
    StripMemorySegment* p = stripMemoryHead;

    while ((p != NULL) && (!p->avail))
        p = p->next;

    return (p != NULL);
}

void StripMemoryManager::WaitMemoryAvail(unsigned int jobID)
{

    if (!IsStripMemoryAvail())
    {
        #if PRINT_COLLATION == BIND
        nvee_value_type TmpCollationValue;
        dmReadVariable(JOB_QUANTITY, &TmpCollationValue, NVEE_Current);

        if (TmpCollationValue.ValueNumber > 1)
        {
            if (stripStreamHead->jobID == jobID)
            {
                ERRMGR_setErrorCode(emWARNING, MEMMGR, COLLATION_CANCEL);
                FR_cancelCollation();
            }
        }
        #endif

        while (1)
        {
            struct timespec waittime;
            int result = 0;
            waittime.tv_nsec = 250000000; //250ms
            waittime.tv_sec  = 0;
            CHECKPOINTA("Wait memory for new page");
            DumpStripStreams();
            result = sem_timedwait(&free_segment_sem, &waittime);
            if (result == ERROR)
            {
                if (ERRMGR_isJobCancel())
                    break;
            }
            else
            {
                sem_post(&free_segment_sem);
                break;
            }
        }
        CHECKPOINTA("Got memory for new page");

    }
}

void StripMemoryManager::BeginStripStream(unsigned int jobID, unsigned int pageID)
{
#ifndef UT_TEST
    // Since we're going to need memory we are going to lock the printmem
    // datadownload semaphore:
    GetPrintMemSem();
    
  #ifdef AIO_POOL_SHARE
    GetAiOPoolOwnership();
  #endif    
#endif

    MemoryStateNotify(STRIP_MEMORY_STAT_NORMAL);
    Lock();
    StripMemorySegment* segment = GetSegment();

    StripStream* stream = (StripStream*)segment->startAddress;
    stream->jobID = jobID;
    stream->pageID = pageID;
    stream->jobCanceledbit = false;
    stream->cancelLock = false;
    stream->numSegments = 1;
    stream->segmentsHead = segment;
    stream->currentSegment = segment;
    stream->numStrips = 0;
#if PRINT_COLLATION == BIND
    nvee_value_type TmpCollationValue;
    dmReadVariable(JOB_QUANTITY, &TmpCollationValue, NVEE_Current);
    stream->CollationValue = TmpCollationValue.ValueNumber;
#endif
    
    // Check PRSS for rendering resolution setup in system.
    OutputResolutionType resolution = eRESOLUTION_Unknown;
    resolution = GetOutputResolution();

    // Get the desired system output bpp from the PRSS.
    OutputBPPEnum outputBPP = eUnknown_OUTPUT_BPP;
    outputBPP = GetOutputBitsPerPixel();

    if ((resolution == eRESOLUTION_300) || (resolution == eRESOLUTION_300x400) || (resolution == eRESOLUTION_300x600))
    {
        if (outputBPP == e2_OUTPUT_BPP)
        {
            stream->stripType = STRIP_300x2BIT;
        }
        else if (outputBPP == e1_OUTPUT_BPP)
        {
            stream->stripType = STRIP_300x1BIT;
        }
        else if (outputBPP == e8_OUTPUT_BPP)
        {
            stream->stripType = STRIP_300x8BIT;
        }
    }
    else
    {
        ScalingModes scaleMode = GetCurrentResolutionScalingMode();
        if (scaleMode == eNoScaling600x600x1)
        {
            // This case handles majority of incoming jobs identified as slow-speed based on Media Size/Type in EI query during GetImagingEnv()
            stream->stripType = STRIP_600x1BIT;
        }
        else if (scaleMode == eNoScaling600x600x8)
        {
            // This is a custom case handled via same path as above, but for 8bpp engines.
            stream->stripType = STRIP_600x8BIT;
        }
        else
        {
            // Handles remainder of jobs that will be scaled.
            if (outputBPP == e2_OUTPUT_BPP)
            {
                stream->stripType = STRIP_600x2BIT;
            }
            else if (outputBPP == e1_OUTPUT_BPP)
            {
                stream->stripType = STRIP_600x1BIT;
            }
            else if (outputBPP == e8_OUTPUT_BPP)
            {
                stream->stripType = STRIP_600x8BIT;
            }
        }
    }

    memset(stream->strips, 0x0, sizeof(stream->strips));
    stream->next = NULL;

    NewStripStream(stream);
    Allocate(sizeof(StripStream));
    UnLock();
}

void StripMemoryManager::NewStripStream(StripStream* stream)
{

    if (stripStreamHead == NULL)
        stripStreamHead = stream;
    else
    {
        StripStream* p = stripStreamHead;
        while (p->next != NULL)
            p = p->next;

        p->next = stream;
    }

    CHECKPOINTA("Setting currentStripStream as new stream - 0x%x", stream);
    currentStripStream = stream;

}

void StripMemoryManager::DeleteStripStream(StripStream* stream)
{
    CHECKPOINTB("Dumping stream 0x%x", stream);
    stripStreamHead = stream->next;
    CHECKPOINTB("New stripStreamHead: 0x%x", stripStreamHead);
}

void StripMemoryManager::FreeStripsStream(unsigned int jobID, unsigned int pageID)
{
    Lock();

    StripStream* stream = stripStreamHead;
    CHECKPOINTB("stream %x, %x", stream, NULL);
    if (stream == NULL)
    {
        UnLock();
        return;
    }

    if ((stream->jobID != jobID) || (stream->pageID != pageID))
    {
        CHECKPOINTB("stream->jobID %d, stream->pageID %d", stream->jobID, stream->pageID);
        CHECKPOINTB("jobID %d, pageID %d", jobID, pageID);
        StripStream* p = stripStreamHead;
        StripStream* q = p->next;

        while (q != NULL)
        {
            CHECKPOINTB("stream->jobID %d, stream->pageID %d", q->jobID, q->pageID);
            if ((q->jobID == jobID) && (q->pageID == pageID))
                break;

            p = q;
            q = q->next;
        }

        if (q != NULL)
        {
            if (q == currentStripStream)
            {
                CHECKPOINTA("Setting currentStripStream as NULL");
                currentStripStream = NULL;
            }

            p->numSegments += q->numSegments;
            p->next = q->next;

        }
        else
        {
            CHECKPOINTA("jobID %d, pageID %x", jobID, pageID);
            assert(0);
        }
        
        DumpStripStreams();
        UnLock();
        return;
    }

    if (stream == currentStripStream)
    {
        CHECKPOINTA("Resetting currentStripStream to NULL");
        currentStripStream = NULL;
    }

    StripMemorySegment* segment = stream->segmentsHead;
    int numOfSegments = stream->numSegments;

    DeleteStripStream(stream);
    FreeSegments(segment, numOfSegments);
    DumpStripStreams();
    UnLock();
}

void StripMemoryManager::ReturnSegments(StripMemorySegment* segment, int num)
{
    int final_segments_num = num;
    final_segments_num -= CombineSegments(segment, num);

    for (int i = 0; i < final_segments_num; i++)
    {
        segment->currentAddress = segment->startAddress;
        segment->avail = true;
        segment = segment->next;
        #ifndef UT_TEST
        CHECKPOINTB("Posting free_segment_sem");
        sem_post(&free_segment_sem);
        #endif

    }

}

void StripMemoryManager::CancelStripsStream(unsigned int jobID, unsigned int pageID)
{
    Lock();

    if (stripStreamHead == NULL)
    {
        UnLock();
        return;
    }


    StripStream* p = stripStreamHead;
    StripStream* q = p->next;

    while ((q != NULL)&&(q->next != NULL))
    {
        p = q;
        q = p->next;
    }

    if (q == NULL)
    {
        q = stripStreamHead;
    }

    CHECKPOINTB("stream %x, %x", q, NULL);

    if (q == currentStripStream)
    {
        CHECKPOINTA("Setting currentStripStream to NULL");
        currentStripStream = NULL;
    }

    if ((q->jobID != jobID) || (q->pageID != pageID))
    {
        UnLock();
        return;
    }

    if (q == stripStreamHead)
    {
        stripStreamHead = NULL;
    }
    else if (q == p->next)
    {
        p->next = NULL;
    }
    else
    {
        assert(0);
        UnLock();
        return;
    }

    ReturnSegments(q->segmentsHead, q->numSegments);
    DumpStripStreams();
    
    UnLock();

    ReturnPrintMemSem();
    
#ifdef AIO_POOL_SHARE
    ReturnAiOPool(); 
#endif
}


void StripMemoryManager::EndStripStream()
{
    Lock();

    if (currentStripStream != NULL)
        SplitSegment(currentStripStream->currentSegment);

    CHECKPOINTA("Resetting currentStripStream to NULL");
    currentStripStream = NULL;
    UnLock();
}

uint32 StripMemoryManager::GetScratchBuffer(int estimateSize)
{
    Lock();
    if (currentStripStream->currentSegment->endAddress - currentStripStream->currentSegment->currentAddress < estimateSize)
    {

        if (IsMemoryAvailForStream(currentStripStream))
        {
            StripMemorySegment* segment = GetNextSegment(currentStripStream);
            if(segment == NULL){
                UnLock();
                return NULL;
            }
            currentStripStream->numSegments++;
            currentStripStream->currentSegment = segment;

            if (segment->endAddress - segment->startAddress < estimateSize)
            {
                assert(0);
            }
        }
        else
        {
            UnLock();
            return NULL;
        }
    }
    
    // Sanitize this data in case it was touched by something else...
    memset((void *)currentStripStream->currentSegment->currentAddress, 0x0, estimateSize);
    UnLock();
    
    return currentStripStream->currentSegment->currentAddress;
}

#ifndef UT_TEST

void StripMemoryManager::VerifyStripType(ImageStripType* strip, int numPlane)
{
    ImageBlock* block = strip->tPlane_Header[SCRATCH_BUFF_PLANE];

    switch (currentStripStream->stripType)
    {
        case STRIP_600x1BIT:
            if (block->bitsPerPixel != 1)
            {
                strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = HTONLY_BLOCK_TYPE;
                Backward(strip, numPlane);
                strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = IMAGE_BLOCK_TYPE;
            }
            break;

        case STRIP_300x1BIT:

            if ((block->bitsPerPixel != 1) || (block->xRes != 300))
            {
                strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = HTONLY_BLOCK_TYPE;
                Backward(strip, numPlane);
                strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = IMAGE_BLOCK_TYPE;
            }
            break;

        default:
            return;

    }
}

/* 
 * Recursive function that cycles through dynamically supported low memory pipe operations. This function is expected to yeild one of the following results
       1. Apply low mem conditions and either get the necessary memory or attempt to flush pages before failing and returning.
       2. When low mem conditions aren't supported, attempt to flush pages before returning a failure.
 */
uint32 StripMemoryManager::ProcessLowMemCycles(const int& estimateSize, const LowMemPipeAbilitiesEnum& lowMemPipeAbilities, ImageStripType* strip, const int& numPlane, uint32& result)
{
    static bool completeCycles = false;
    if( JobIsCanceled(currentStripStream->jobID))
    {
        completeCycles = true;
        return result;
    }

    if ((result == NULL) && !completeCycles)
    {
        if (lowMemPipeAbilities == eLOW_MEM_NO_OP)
        {
            //FlushAllPages(); // No replay when low mem conditions not supported. Try flushing pages before bailing out.
            completeCycles = true;
            //result = GetScratchBuffer(estimateSize);
            //ProcessLowMemCycles(estimateSize, lowMemPipeAbilities, strip, numPlane, result);
            result = NULL;
            CHECKPOINTA("stripmemorypool memory out, It is impossible, add a assert to detect this case");
            assert(0); 
        }
        else
        {
            switch (currentStripStream->stripType)
            {
                case STRIP_300x2BIT:
                    if ((lowMemPipeAbilities == e2_TO_1_BPP_AND_600_to_300_DPI ) || (lowMemPipeAbilities == e2_BPP_TO_1_BPP_ONLY ))
                    {
                        MemoryStateNotify(STRIP_MEMORY_STAT_LOWMEM_I);
                        currentStripStream->stripType = STRIP_300x1BIT;
                        ReplayStrips(strip, numPlane);
                    }
                    else
                    {
                        FlushAllPages();
                        completeCycles = true;
                    }
                    break;
                case STRIP_600x2BIT:
                    if ((lowMemPipeAbilities == e2_TO_1_BPP_AND_600_to_300_DPI ) || (lowMemPipeAbilities == e2_BPP_TO_1_BPP_ONLY ))
                    {
                        MemoryStateNotify(STRIP_MEMORY_STAT_LOWMEM_I);
                        currentStripStream->stripType = STRIP_600x1BIT;
                        ReplayStrips(strip, numPlane);
                    }
                    else
                    {
                        FlushAllPages();
                        completeCycles = true;
                    }
                    break;
                case STRIP_600x1BIT:
                    if ((lowMemPipeAbilities == e2_TO_1_BPP_AND_600_to_300_DPI ) || (lowMemPipeAbilities == e600_TO_300_DPI_ONLY))
                    {
                        MemoryStateNotify(STRIP_MEMORY_STAT_LOWMEM_II);
                        currentStripStream->stripType = STRIP_300x1BIT;
                        ReplayStrips(strip, numPlane);
                    }
                    else
                    {
                        FlushAllPages();
                        completeCycles = true;
                    }
                    break;
                case STRIP_300x1BIT:
                default:
                    FlushAllPages();
                    completeCycles = true;
                    break;
            }
            
            result = GetScratchBuffer(estimateSize);
            ProcessLowMemCycles(estimateSize, lowMemPipeAbilities, strip, numPlane, result);
        }
    }

    // reset the compeleteCyles flag
    completeCycles = false;
    return result;
}

uint32 StripMemoryManager::DoAction(ImageStripType* strip, int numPlane)
{
    uint32 result = 0;
    ImageBlock* src = strip->tPlane_Header[SCRATCH_BUFF_PLANE];
    int estimateSize = GetAlignedSize(src->compressSize) + GetAlignedSize(sizeof(ImageBlock));

    result = GetScratchBuffer(estimateSize);

    // Get the LowMemPipe abilities to see what conditions are supported
    LowMemPipeAbilitiesEnum lowMemPipeAbilities = GetLowMemPipeAbilities(); 
#ifndef NDEBUG
    if (lowMemPipeAbilities != eLOW_MEM_NO_OP)
    {
        CAppVariableManager VarMgr;
        IAppVariable* DMPrintVars = VarMgr.getVariableInterface(DM_PRINT_VARIABLE_NAME);
        uint32 stripsToLowMemoryI = 0, stripsToLowMemoryII = 0;
        if(FAILED(DMPrintVars->get(eStripsToLowMemoryI, &stripsToLowMemoryI)))
        {
            CHECKPOINTA("ERROR - Unable to get eStripsToLowMemoryI");
            assert(false);
        }
        if(FAILED(DMPrintVars->get(eStripsToLowMemoryII, &stripsToLowMemoryII)))
        {
            CHECKPOINTA("ERROR - Unable to get eStripsToLowMemoryII");
            assert(false);
        }
        CHECKPOINTB("stripsToLowMemoryI %d, stripsToLowMemoryII %d", stripsToLowMemoryI, stripsToLowMemoryII);
        CHECKPOINTB("numStrips %d", currentStripStream->numStrips);
        // 0 is no allowed
        if ((stripsToLowMemoryI != 0) && (stripsToLowMemoryII != 0))
        {
            if (strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class == IMAGE_BLOCK_TYPE)
            {
                uint32 numStrips = currentStripStream->numStrips;
                if ((numStrips >= stripsToLowMemoryI) && (numStrips < stripsToLowMemoryI + stripsToLowMemoryII))
                {
                    if ((lowMemPipeAbilities == e2_TO_1_BPP_AND_600_to_300_DPI) || (lowMemPipeAbilities == e2_BPP_TO_1_BPP_ONLY))
                    {
                        if ((STRIP_MEMORY_STAT_LOWMEM_I != GetMemStat()) && (currentStripStream->stripType != STRIP_300x1BIT))
                        {
                            CHECKPOINTA("TEST: switch to LOWMEM I\n");
                            result = NULL;
                        }
                    }
                }
                else if (numStrips >= (stripsToLowMemoryI + stripsToLowMemoryII))
                {
                    if ((lowMemPipeAbilities == e2_TO_1_BPP_AND_600_to_300_DPI) || (lowMemPipeAbilities == e600_TO_300_DPI_ONLY))
                    {
                        if (STRIP_MEMORY_STAT_LOWMEM_II != GetMemStat())
                        {
                            CHECKPOINTA("TEST: switch to LOWMEM II\n");
                            result = NULL;
                        }
                    }
                }
            }
        }
    }
#endif
    if (result == NULL)
    {
        if (src->Class == REPLAY_BLOCK_TYPE)
        {
            CHECKPOINTA("replay strip is impossible to memory ouut");
            assert(0);
        }
#ifndef NDEBUG
        SPI_DEBUG_STDERR("pageID: %d\n", currentStripStream->pageID);
        SPI_DEBUG_STDERR("jobID: %d\n", currentStripStream->jobID);
        SPI_DEBUG_STDERR("numStrips: %d\n", currentStripStream->numStrips);
        SPI_DEBUG_STDERR("STRIP_MEMORY_STAT: LOWMEM %d => ", GetMemStat());
        SPI_DEBUG_STDERR("stripType: %x", currentStripStream->stripType);
#endif

        result = ProcessLowMemCycles(estimateSize, lowMemPipeAbilities, strip, numPlane, result);
        CHECKPOINTB("Completed ProcessLowMemCycles with result = %d", result);

#ifndef NDEBUG
        SPI_DEBUG_STDERR("LOWMEM %d\n", GetMemStat());
#endif
    }
    else
        VerifyStripType(strip, numPlane);

    return result;

}
#endif



void StripMemoryManager::Allocate(int size)
{
     size = GetAlignedSize(size);

     if (currentStripStream->currentSegment->endAddress - currentStripStream->currentSegment->currentAddress < size)
         assert(0);

     currentStripStream->currentSegment->currentAddress += size;
}

void* StripMemoryManager::AllocateStrip(int size, unsigned int jobID, unsigned int pageID)
{
    Lock();
    size = GetAlignedSize(size);
    
    
    if (currentStripStream->currentSegment->endAddress - currentStripStream->currentSegment->currentAddress < size)
    {
        UnLock();
        return NULL;
    }
    
    if (jobID != currentStripStream->jobID)
    {
        assert(0);
    }
    
    if (pageID != currentStripStream->pageID)
    {
        assert(0);
    }
    
    
    void* addr = (void*)currentStripStream->currentSegment->currentAddress;
    
    currentStripStream->strips[currentStripStream->numStrips++] = (ImageBlockType *)currentStripStream->currentSegment->currentAddress;
    currentStripStream->currentSegment->currentAddress += size;

    UnLock();
    
    return addr;
}

void StripMemoryManager::LockPipe(void)
{
    CHECKPOINTA("Locking the mempipe_mutex");
#ifndef UT_TEST
    pthread_mutex_lock(&mempipe_mutex);
#endif
}

void StripMemoryManager::UnLockPipe(void)
{
    CHECKPOINTA("Unlocking the mempipe_mutex");
#ifndef UT_TEST
    pthread_mutex_unlock(&mempipe_mutex);
#endif
}

void StripMemoryManager::ResetStripStream()
{
    Lock();
    StripMemorySegment* p = currentStripStream->segmentsHead;
    for (int i = 0; i < currentStripStream->numSegments; i++)
    {
        if (i == 0)
        {
            p->currentAddress = (unsigned int)currentStripStream->strips[0]; //strip buffer address reset to first strip location
        }
        else
        {
            p->avail = true;
            #ifndef UT_TEST
            CHECKPOINTB("Posting free_segment_sem");  
            sem_post(&free_segment_sem);
            #endif
        }
        p = p->next;
    }

    currentStripStream->numSegments = 1;
    currentStripStream->numStrips = 0;
    currentStripStream->currentSegment = currentStripStream->segmentsHead;

    UnLock();
}


bool StripMemoryManager::IsMemoryAvailForStream(StripStream* stream)
{
#if PRINT_COLLATION == BIND
    if (stream->CollationValue > 1) {
        if (stripStreamHead->jobID == stream->jobID)
        {
            ERRMGR_setErrorCode(emWARNING, MEMMGR, COLLATION_CANCEL);
            FR_cancelCollation();
        }

        return true;
    }
#endif
    if (stripMemoryHead != stream->segmentsHead)
        return true;

    if ((stream->currentSegment->next != NULL) && (stream->currentSegment->next->avail))
        return true;

    return false;
}

void StripMemoryManager::ReplayStrips(ImageStripType* strip, int numPlane)
{
    ImageBlock block;
    ImageStripType replay_strip;
    
    //We need to release the allocation lock so when we replay the strips,
    //the system can re-acquire the lock when recursively re-entering the 
    //low mem fx pipe. 
    this->AllocationUnLock();
    
    StripStream* stream = currentStripStream;
    stream->cancelLock = true;
    int numStrips = stream->numStrips;

    ResetStripStream();

    for (int i = 0; i < numStrips; i++)
    {
        memcpy(&block, stream->strips[i], sizeof(ImageBlock));

        // sanitize strip for replaying.
        bzero(&replay_strip, sizeof(ImageStripType));
        
        replay_strip.tPlane_Header[0] = NULL;
        replay_strip.tPlane[0] = NULL;

        replay_strip.tPlane_Header[SCRATCH_BUFF_PLANE] = &block;
        replay_strip.tPlane[SCRATCH_BUFF_PLANE] = strip->tPlane[R_PLANE];

        Backward(&replay_strip, numPlane);
    }

    strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = HTONLY_BLOCK_TYPE;
    Backward(strip, numPlane);
    strip->tPlane_Header[SCRATCH_BUFF_PLANE]->Class = IMAGE_BLOCK_TYPE;
    
    // Fix issue wherein EIC performance upgrade removed PDL's sanitization operation:
    //     the PDL now has to rely on all client's using it's memory to cleanup after themselves.
    uint32 sanitizationSize = strip->tPlane_Header[R_PLANE]->wordCount[R_PLANE];
    CHECKPOINTA("Sanitizing 240KB buffer's first %d bytes, since they were used for replayStrips", sanitizationSize);
    CHECKPOINTA("Strip starts at: 0x%x", &strip->tPlane[R_PLANE]);
    memset(strip->tPlane[R_PLANE], 0x0, sanitizationSize);

    stream->cancelLock = false;
    this->AllocationLock();
}

extern "C" void TraceCallStack(uint32 maxDepth);

void StripMemoryManager::Lock(void)
{
    CHECKPOINTA("waiting on stream_op_sem");
    //TraceCallStack(20);
    #ifndef UT_TEST
    sem_wait(&stream_op_sem);
    #endif
    CHECKPOINTA("got stream_op_sem");
}

void StripMemoryManager::UnLock(void)
{
    CHECKPOINTA("Posting stream_op_sem");
    //TraceCallStack(20);
    #ifndef UT_TEST
    sem_post(&stream_op_sem);
    #endif
}

void StripMemoryManager::DetectStripsCorruption(int numToIgnore)
{
    // This function goes off and detects if there is a potential corruption that has occurred in the strips array for
    // the current stripstream that is under construction.

    StripStream * streamHead = currentStripStream;

    for(int i=0; i< ((streamHead->numStrips) - numToIgnore); i++)
    {
        CHECKPOINTA("Strip #%d", i);
        ImageBlock* blockPtr = streamHead->strips[i];
        CHECKPOINTA("fLink: 0x%x, NULL: 0x%x", blockPtr->fLink, NULL);
        assert((blockPtr->fLink) == NULL);
    }
}   

void StripMemoryManager::PageLock(void)
{
    CHECKPOINTA("waiting on stream_page_op_sem");
    //TraceCallStack(20);
    #ifndef UT_TEST
    sem_wait(&stream_page_op_sem);
    #endif
    CHECKPOINTA("got stream_page_op_sem");
}

void StripMemoryManager::PageUnLock(void)
{
    CHECKPOINTA("Posting stream_page_op_sem");
    //TraceCallStack(20);
    #ifndef UT_TEST
    sem_post(&stream_page_op_sem);
    #endif
}

ubyte * StripMemoryManager::GetStartOfCurrentPage(void)
{
    // return the address of the start of the current stripStream block. Otherwise the entire strip buffer...
    return (ubyte*)GetStripMemoryStartAddress();
}

ubyte * StripMemoryManager::GetEndOfCurrentPage(void)
{
    // return the ending address of the current stripstream block. Otherwise the entire strip buffer...
    return (ubyte*)(GetStripMemoryStartAddress() + MEMORY_FOR_COMPRESSED_STRIP_DATA);
}

void StripMemoryManager::WriteFinalColumnData(ImageBlockType * DestinationMemory, ubyte Class, int compressSize, uint32 * sourceDataPlane, uint32 wordCountDataPlane, uint32 * sourceObjectPlane, uint32 wordCountObjectPlane)
{
    // Write in the current data into the strip before it gets corrupted by something else.
    // Doing it inside of here prevents anybody from getting a lock on the system and potentially corrupting anything from the system.
    Lock();
    CHECKPOINTA("Writing out final data for ImageBlockType * 0x%x", DestinationMemory);
    DestinationMemory->Class = Class;
    DestinationMemory->compressSize = compressSize;
    DestinationMemory->source[0] = sourceDataPlane;
    DestinationMemory->wordCount[0] = wordCountDataPlane;
    DestinationMemory->source[OBJ_PLANE] = sourceObjectPlane;
    DestinationMemory->wordCount[OBJ_PLANE] = wordCountObjectPlane;

    UnLock();
}

void StripMemoryManager::AllocationLock()
{
    // Lock the stripMemoryManager's allocation semaphore.
    // Sadly we need to do this because we have to post-allocate data from the strip-store.
    CHECKPOINTA("waiting on stream_alloc_sem");
    TraceCallStack(20);
    #ifndef UT_TEST
    sem_wait(&stream_alloc_sem);
    #endif
    CHECKPOINTA("got stream_alloc_sem");
}
    
void StripMemoryManager::AllocationUnLock()
{
    // Companion UnLock function to release the allocation sempahore.
    CHECKPOINTA("Posting stream_alloc_sem");
    TraceCallStack(20);
    #ifndef UT_TEST
    sem_post(&stream_alloc_sem);
    #endif
}

bool StripMemoryManager::IsNewPage(unsigned int jobID, unsigned int pageID)
{
    Lock();
    if (currentStripStream == NULL)
    {
        UnLock();
        return true;
    }
    else
    {
        if ((currentStripStream->jobID == jobID) && (currentStripStream->pageID == pageID))
        {
            UnLock();
            return false;
        }
    }

    UnLock();
    return true;
}

bool StripMemoryManager::CheckInternalValidity()
{
    int totalSize = 0;
    StripMemorySegment* p = stripMemoryHead;
    StripMemorySegment* q;

    if (p == NULL)
        return false;

    while (q = p->next, q != NULL)
    {
        if ((p->startAddress > q->startAddress) && (q->startAddress != GetStripMemoryStartAddress() + GetAlignedSize(sizeof(StripMemorySegment))))
        {
            return false;
        }


        totalSize += p->endAddress - p->startAddress + GetAlignedSize(sizeof(StripMemorySegment));

        p = q;
    }

    totalSize += p->endAddress - p->startAddress + GetAlignedSize(sizeof(StripMemorySegment));

    if (totalSize != totalMemorySize) return false;

    return true;
}

StripStream* StripMemoryManager::GetStripStream(unsigned int jobID, unsigned int pageID)
{
    Lock();
    if ((currentStripStream)&&(currentStripStream->jobID == jobID)&&(currentStripStream->pageID == pageID))
    {
        UnLock();
        return currentStripStream;
    }

    UnLock();
    return NULL;
}

bool StripMemoryManager::GetStripStreamConfig(unsigned int jobID, unsigned int pageID, uint16& xRes, uint16& yRes, uint8& bitsPerPixel)
{
    if ((currentStripStream) && (currentStripStream->jobID == jobID) && (currentStripStream->pageID == pageID))
    {
        // Ask the PRSS what the current OutputResolution is set to:
        OutputResolutionType currResolution = GetOutputResolution();

        switch (currResolution)
        {
            case eRESOLUTION_1200:
                yRes = 1200;
                break;
                
            case eRESOLUTION_600x300:
                yRes = 300;
                break;
                
            case eRESOLUTION_600x400:
                yRes = 400;
                break;
                
            case eRESOLUTION_600:
                yRes = 600;
                break;
                
            default:
                CHECKPOINTA("Didn't find a real resolution!?");
                assert(false);
                break;
        }
        
        switch (currentStripStream->stripType)
        {
            case STRIP_300x2BIT:
                xRes = 300;
                bitsPerPixel = 2;
                return true;

            case STRIP_300x8BIT:
                xRes = 300;
                bitsPerPixel = 8;
                return true;
                
            case STRIP_600x2BIT:
                xRes = 600;
                bitsPerPixel = 2;
                return true;
                
            case STRIP_600x1BIT:
                xRes = 600;
                bitsPerPixel = 1;
                return true;
                
            case STRIP_600x8BIT:
                xRes = 600;
                bitsPerPixel = 8;
                return true;
                
            case STRIP_300x1BIT:
                xRes = 300;
                bitsPerPixel = 1;
                return true;
                
            default:
                CHECKPOINTA("Undefined StripStream type!?");
                assert(0);        
        }
    }
    else
    {
        CHECKPOINTA("Unable to find valid currentStripStream in pipe");
        xRes = 0;
        yRes = 0;
        bitsPerPixel = 0;
    }

    return false;
}

bool StripMemoryManager::GetStripStreamStripWidth(unsigned int jobID, unsigned int pageID, uint16& stripWidth)
{
    Lock();
    if ((currentStripStream)&&(currentStripStream->jobID == jobID)&&(currentStripStream->pageID == pageID))
    {
        if ((currentStripStream->strips[0]) && (currentStripStream->strips[0]->width != 0))
        {
            stripWidth = currentStripStream->strips[0]->width;
            UnLock();
            return true;
        }
    }

    UnLock();
    return false;
}

boolean StripMemoryManager::IsStripStreamInUse(void)
{
    CHECKPOINTA("StripMemory Manager is (bool)(%d) in use", (ubyte)this->printmemSemTaken);
    return this->printmemSemTaken;
}

int  StripMemoryManager::GetAlignedSize(int size)
{
    size = ( size + alignAllocationsTo - 1 ) & (~(alignAllocationsTo-1));
    return size;
}

void  StripMemoryManager::DumpStripStreams()
{

    StripStream* stream = stripStreamHead;

    if (stream != NULL)
    {
        while (stream != NULL)
        {
            CHECKPOINTB("stream jobID %d", stream->jobID);
            CHECKPOINTB("stream pageID %d", stream->pageID);
            CHECKPOINTB("stream numSegments %d", stream->numSegments);

            StripMemorySegment * segment = stream->segmentsHead;
            for (int i = 0; i < stream->numSegments; i++)
            {
                CHECKPOINTB("segment->startAddress %x", segment->startAddress);
                CHECKPOINTB("segment->endAddress %x", segment->endAddress);
                CHECKPOINTB("segment->currentAddress %x", segment->currentAddress);
                segment = segment->next;
            }

            stream = stream->next;
        }
    }

    CHECKPOINTB("Segments Dump");
    StripMemorySegment * segment = stripMemoryHead;
    int seg_count = 0;
    while (segment != NULL)
    {
        CHECKPOINTB("segment->startAddress %x", segment->startAddress);
        CHECKPOINTB("segment->endAddress %x", segment->endAddress);
        CHECKPOINTB("segment->currentAddress %x", segment->currentAddress);
        if (segment->avail)
        {
            seg_count++;
        }
        segment = segment->next;
    }

    #ifndef UT_TEST
    int count = 0;
    sem_getvalue(&free_segment_sem, &count);
    CHECKPOINTB("seg_count %d, free_segment_sem count %d", seg_count, count);
    #endif
}

void* StripMemoryManager::AllocateConfiguredStrip(int size)
{
    int aligned_size = GetAlignedSize(size);

    SetTotalMemorySize(GetTotalSpaceInBytes() - aligned_size);

    return (void*)(GetStripMemoryStartAddress() + GetTotalSpaceInBytes());


}


unsigned int StripMemoryManager::GetStripMemoryStartAddress(void)
{
    return ((unsigned int)memoryBuffer + (alignAllocationsTo - 1))&(~(alignAllocationsTo - 1));
}


void StripMemoryManager::FreeChunkyBuffers(void)
{
    ResetStripStream();
}


void* StripMemoryManager::AllocChunkyBuffer(int size)
{
    void * addr =  (void*) GetScratchBuffer(size);
    Allocate(size);
    return addr; 
}
