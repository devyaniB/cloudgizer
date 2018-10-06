/*
Copyright (c) 2017 DaSoftver LLC.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

// 
// Memory handling, including garbage collector
// CLD memory is made for web requests. Most of the time (if not all),
// memory is only allocated and at the end of a request, CLD will de-allocate 
// it. Always use cld_ functions, and unless absolutely needed, NEVER free
// memory allocated - it will be handled automatically at the end of the request.
// Uses stdlibc malloc, but keeps track of all memory
// usage. Memory is periodically released in entirety, eliminating memory fragmentation
// Release happens after servicing each request
//


#include "cld.h"

// functions
CLD_MEMINLINE int add_mem (void *p);
CLD_MEMINLINE void *vmset (void *p, int r, size_t sz);
CLD_MEMINLINE int cld_get_memory (void *ptr);
void __cld_show_bad_mem (void *ptr);
const char *cld_out_mem_mess = "Out of memory for [%d] bytes";

// Common empty string constant, used for initialization
// When a variable has this value, it means it is freshly initialized and it
// will be either re-assigned, or allocated.
char *CLD_EMPTY_STRING="";



// sizeof(int)+2+sizeof(size_t) is the overhead for CLD memory checksum before each block(see below)
// however, since all memory needs to be 16 bytes align (as of now), we need to have that
// much memory for the overhead, so that the actual memory served is always 16-bytes aligned
// What is important is that our overhead is smaller, i.e. sizeof(int)+2+sizeof(t)<=k*16
// Another important requirement is that int MUST always be on a 4 byte boundary (or sizeof(int))
// whatever it is. If it is not, CPU will NOT read it correctly, it may be garbage.
// So our requirement is sizeof(int)+sizeof(int)+sizeof(size_t)<=k*16 (this time sizeof(int) instead of 2). And 
// for that reason CLDALIGN is 2*sizeof(int)+sizeof(size_t)

// change this if in the future CPU alignment is 32 bytes alignment,instead of 16 for example! Note that CPU 
// alignment is INDEPENDENT of sizeof(int). 16 here is the CPU alignment. It must be minimum that.
// But if the amount needed is greater than that, then it must be a multiple of 16, as computed below.
#define CLDCPUALIGN (16)
#define CLDMULTALIGN(x) (((x)/CLDCPUALIGN+(x%CLDCPUALIGN !=0 ? 1:0)) * CLDCPUALIGN)
#define CLDALIGN (CLDMULTALIGN(2*sizeof(int)+sizeof(size_t)))


// static variables used in memory handling
// We delete old's request memory at the very start of a new request (in generated code before any user code).
// Because of static designation here, no module can actually directly read the memory (of itself or other modules).
//
static void **vmmem = NULL;
static int vmmem_curr = 0;
static int vmmem_tot = 0;

// determines the size of the block allocated (and the size of consequent expansions) for the memory
// block that keeps all pointers to allocated blocks.
#define CLDMSIZE 128

// 
// must be called at the very beginning of program. Initializes memory and 
// cleans up any memory left over from a previous request.
//
void cld_memory_init ()
{
    // cleaning of memory CANNOT be done at the end of generated code, because it is NOT the end - some memory
    // we reserved here WILL be used in apache after our "main" function quits. So cld_done should NOT be
    // used anywhere but here, which is at the beginning of the FOLLOWING request. 
    cld_done ();

    vmmem = calloc (vmmem_tot = CLDMSIZE, sizeof (void*));
    if (vmmem == NULL) cld_report_error ("Out of memory");
    vmmem_curr = 0;
}

// 
// Add point to the block of memory. 'p' is the memory pointer (allocated elsewhere here) added.
// Returns the index in memory block where the pointer is.
// Once a block of pointers is exhausted, add another block. We do not increase the blocks
// size size requests are generally small and typically do not need much more memory, and increasing
// the block size might cause swaping elsewhere.
//
CLD_MEMINLINE int add_mem (void *p)
{
    int r;
    vmmem[r = vmmem_curr] = p;
    vmmem_curr++;
    if (vmmem_curr >= vmmem_tot)
    {
        vmmem_tot += CLDMSIZE;
        vmmem = realloc (vmmem, vmmem_tot * sizeof (void*));
        if (vmmem == NULL)
        {
            cld_report_error (cld_out_mem_mess, vmmem_tot*sizeof(void*));
        }
    }
    return r;
}

// 
// Adds pointer to our block of memory. 'p' is the pointer allocated elsewhere.
// 'r' is the index in the block of memory where p is. sz is the size of the memory to
// which p points. 
// The memory returned is the actually a pointer to useful memory (that a CLD program can use). We place
// some information at the beginning of the memory pointed to by 'p': two magic bytes, the reference to the 
// index in the block of memory where p is, and the size of the memory block.
//
CLD_MEMINLINE void *vmset (void *p, int r, size_t sz)
{
    // sizeof(int) must be greater than # of bytes written prior to memcpy..of r, in this case we write only 2 bytes
    // two bytes to detect underwrite. We could have done 1 but we have memory to spare due to alignment.
    *(unsigned char *)p = 193;
    *((unsigned char *)p+1) = 37;
    memcpy ((unsigned char*)p + sizeof(int), &r, sizeof (int));
    memcpy ((unsigned char*)p + 2*sizeof(int), &sz, sizeof (size_t));
    return (unsigned char*)p + CLDALIGN;
}


// 
// __ functions are top-level function that are actually called through macros
//


// 
// input and returns are like malloc().
//
CLD_MEMINLINE void *__cld_malloc(size_t size)
{
    size_t t;
    void *p = malloc (t=size + CLDALIGN+1);
    if (p == NULL) 
    {
        cld_report_error (cld_out_mem_mess, size+CLDALIGN+1);
    }
    // set the byte to detect overwrites, here and elsewhere below
    ((unsigned char*)p)[t-1]=67;
    // add memory pointer to memory block
    int r = add_mem (p);
    // set underwrite detection bytes and index/size of the block
    return vmset(p,r, t);
}

// 
// input and returns are like calloc()
// See malloc for the rest
//
CLD_MEMINLINE void *__cld_calloc(size_t nmemb, size_t size)
{
    size_t t;
    void *p =  malloc (t=(nmemb*size + CLDALIGN+1));
    if (p == NULL) 
    {
        cld_report_error (cld_out_mem_mess, t);
    }
    memset (p, 0, t);
    ((unsigned char*)p)[t-1]=67;
    int r = add_mem (p);
    return vmset (p,r,t);
}

// 
// Trace bad memory for post-mortem examination
// (if program didn't crash prior to this)
//
void __cld_show_bad_mem (void *ptr)
{
    int i;
    for (i=0; i<100;i++)
    {
        CLD_TRACE("Byte [%c] (%d)\n", ((unsigned char*)ptr)[i], (int)(((unsigned char*)ptr)[i]));
    }
}

//
// Get index of pointer in memory block
//
CLD_MEMINLINE int cld_get_memory (void *ptr)
{
    return *(int*)((unsigned char*)ptr-CLDALIGN+sizeof(int));
}

// 
// Assert that memory is correct.
// ptr is the 'usable' memory pointer (used in CLD application)
// Returns index of pointer in memory block or fails assertion. If return value is 0
// this could be CLD_EMPTY_STRING - always check for it before calling this function!
// CLD_EMPTY_STRING is just a constant string, not a heap memory! If ptr is CLD_EMPTY_STRING, 
// do NOT call this function.
// This is used in top-line memory functions (free,realloc) to check
// memory passed is correct.
// It is highly unlikely for an invalid pointer to pass or for valid pointer
// not to pass. No actual memory content is checked.
// block_size is the output holding the size of memory block allocated.
//
CLD_MEMINLINE int cld_check_memory(void *ptr, int *block_size)
{
    // 
    // Empty string is used in memory allocation system as sort of a "NULL"
    // value. It is not a heap memory, but we consider it valid.
    // Also, if it is NULL, consider it zero-sized.
    //
    if (ptr == CLD_EMPTY_STRING || ptr == NULL)
    {
        // 
        // This is NOT a first element in memory array -this is CLD_EMPTY_STRING
        // Always check if ptr is CLD_EMPTY_STRING before calling this function, and if it is
        // do not call it!
        // (We do that everywhere in code)
        //
        //
        // We set block_size to 0, because we use this function to check memory allocated, and
        // that's what's allocated for this (i.e. nothing is allocated).
        //
        *block_size = 0;
        return 0;
    }

    int r = cld_get_memory (ptr);
    //
    // Check if memory index out of range of array of pointers we allocated for memory
    //
    if (r<0 || r>=vmmem_curr)
    {
         CLD_TRACE("Memory pointer out of range, [%d], total memory range [0-%d]", r, vmmem_curr);
         abort();
    }
    // check underwriting
    if ((*((unsigned char*)ptr-CLDALIGN) != 193) || (*((unsigned char*)ptr-CLDALIGN+1) != 37))
    {
        CLD_TRACE("Memory corrupted (before block), memory region shown next");
        __cld_show_bad_mem(ptr-CLDALIGN);
        abort();
    }

    int sz = *(int*)((unsigned char*)ptr-CLDALIGN+2*sizeof(int));
    //
    // Get block memory size if requested
    //
    if (block_size != NULL)
    {
        *block_size = sz;
    }

    // check overwriting and reverse index to be correct
    if ((*((unsigned char*)ptr-CLDALIGN+sz-1) != 67) || ((unsigned char*)(vmmem[r])!=(unsigned char*)(ptr-CLDALIGN)))
    {
        CLD_TRACE("Memory corrupted (after block), memory region shown next");
        __cld_show_bad_mem(ptr-CLDALIGN);
        abort();
    }
    return r;
}

// 
// Input and return the same as for realloc()
// Checks memory to make sure it's valid block allocated here.
//
CLD_MEMINLINE void *__cld_realloc(void *ptr, size_t size)
{
    size_t t;
    //
    // Check if string uninitialized, if so, allocate it for the first time
    // Also, if pointer is a NULL ptr, just allocate memory.
    //
    if (ptr == CLD_EMPTY_STRING || ptr == NULL)
    {
        return __cld_malloc (size);
    }
    int r = cld_check_memory(ptr, NULL);
    vmmem[r] = NULL;
    void *p= realloc ((unsigned char*)ptr-CLDALIGN, t=size + CLDALIGN+1);
    if (p == NULL) 
    {
        cld_report_error (cld_out_mem_mess, size+CLDALIGN+1);
    }
    ((unsigned char*)p)[t-1]=67;
    r = add_mem(p);
    return vmset(p,r, t);
}

// 
// Input and return the same as for free()
// Checks memory to make sure it's valid block allocated here.
//
CLD_MEMINLINE void __cld_free (void *ptr)
{
    //
    // if programmer mistakenly frees up CLD_EMPTY_STRING, just ignore it
    //
    if (ptr == CLD_EMPTY_STRING || ptr == NULL) return;
    int r = cld_check_memory(ptr, NULL);
    vmmem[r] = NULL;
    free ((unsigned char*)ptr-CLDALIGN);
}

// 
// Input and return the same as for strdup()
//
CLD_MEMINLINE char *__cld_strdup (const char *s)
{
   int l = strlen (s);
   char *n = (char*)__cld_malloc (l+1);
   if (n == NULL) 
   {
       cld_report_error (cld_out_mem_mess, l+1);
   }
   memcpy (n, s, l+1);
   return n;
}

// 
// Frees all memory allocated so far.
// This is called at the beginning of a request before memory is allocated again.
// The reason this is NOT called at the end of the request is that web server NEEDS
// some of the allocated memory even after the request ends, for example, the 
// actual web output or header information.
//
void cld_done ()
{
    if (vmmem != NULL)
    {
        int i;
        for (i = 0; i < vmmem_curr; i++)
        {
            if (vmmem[i] != NULL)
            {
                //CLD_TRACE("Freeing [%d] block of memory", i);
                __cld_free ((unsigned char*)vmmem[i]+CLDALIGN);
            }
        }
        //CLD_TRACE("Freeing vmmem");
        free (vmmem);
    }
}

//
// Checks entire memory for overwrites and underwrites quickly by examining magic bytes and pointer
// cross-referencing. 
// To be used for debugging only. CLD will use this in each CLD_TRACE() call regardless of
// whether trace is enabled or not, and if memorycheck is set to "1" in the debug file.
// Note: using this will slow down execution, so to be used ONLY during debugging.
//
void cld_checkmem ()
{
    if (vmmem != NULL)
    {
        int i;
        //
        // Check every cld_ allocated memory block for over-writes and under-writes
        for (i = 0; i < vmmem_curr; i++)
        {
            if (vmmem[i] != NULL)
            {
                cld_check_memory((unsigned char*)vmmem[i]+CLDALIGN, NULL);
            }
        }
    }
}




