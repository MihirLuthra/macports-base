/*
 * dtsharedmemory.c
 *
 * Copyright (c) 2019 The MacPorts Project
 * Copyright (c) 2019 Mihir Luthra <1999mihir.luthra@gmail.com>,
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The MacPorts Project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>


#include "dtsharedmemory.h"


#ifdef HAVE_STDATOMIC_H
#include <stdatomic.h>
#endif

#ifdef HAVE_LIBKERN_OSATOMIC_H
#include <libkern/OSAtomic.h>
#endif


#ifdef HAVE_STDATOMIC_H
static _Atomic(struct SharedMemoryManager *) manager = NULL;
#else
static         struct SharedMemoryManager *  manager = NULL;
#endif



#define FILE_PERMISSIONS 0600 //Permissions for status file and shared memory file

/*Empty comments are placed all over to avoid crazy indentation caused by auto-indent on macro definitions*/

#if HAVE_DECL_ATOMIC_COMPARE_EXCHANGE_STRONG_EXPLICIT

#	define CAS_ptr(old, new, mem) \
/**/		atomic_compare_exchange_strong_explicit(mem, old, new, memory_order_relaxed, memory_order_relaxed)

#	define CAS_size_t(old, new, mem) \
/**/		atomic_compare_exchange_strong_explicit(mem, old, new, memory_order_relaxed, memory_order_relaxed)


#elif \
defined(HAVE_OSATOMICCOMPAREANDSWAPPTR) && \
(\
(  defined(__LP64__) && defined(HAVE_OSATOMICCOMPAREANDSWAP64)) || \
( !defined(__LP64__) && defined(HAVE_OSATOMICCOMPAREANDSWAP32)) \
)

#	define CAS_ptr(old, new, mem) \
/**/		OSAtomicCompareAndSwapPtr((void *) (*old), (void *)new, (void* volatile *)mem)

//If 64 bit machine, use 64 bit CAS for size_t else 32 bit CAS
#	ifdef __LP64__
#		define CAS_size_t(old, new, mem) \
/**/			OSAtomicCompareAndSwap64((int64_t) (*old), (int64_t) (new), (volatile int64_t *) (mem))
#	else
#		define CAS_size_t(old, new, mem) \
/**/			OSAtomicCompareAndSwap32((int32_t) (*old), (int32_t) (new), (volatile int32_t *) (mem))
#	endif

#else

#	error "No compare and swap primitive available."

#endif



/**
 *	Sets bitmap value at given index
 *	While dealing with bitmaps, typecasting is necessary otherwise it leads to errors.
 **/
#define setBitmapAtIndex(bitmap, indexForBitmap) \
	(bitmap | ((size_t)1 << indexForBitmap))



/**
 *	Unsets bitmap value at given index
 *	While dealing with bitmaps, typecasting is necessary otherwise it leads to errors.
 **/
#define unsetBitmapAtIndex(bitmap, indexForBitmap) \
	(bitmap & ~( (size_t)1 << indexForBitmap ))



/**
 *	Gets bitmap value from given index
 *	While dealing with bitmaps, typecasting is necessary otherwise it leads to errors.
 **/
#define getBitmapAtIndex(bitmap, indexForBitmap) \
	((bitmap >> indexForBitmap) & (size_t)1)



/**
 *
 *	PADDING_BYTES are added so that recycled offsets can use an odd offset
 *	by adding 1. PADDING_BYTES create extra space so 1 can be added to recycled offsets.
 *
 **/
#if !(DISABLE_DUMPING_AND_RECYCLING)
#	define PADDING_BYTES 2
#else
#	define PADDING_BYTES 0
#endif



/**
 *	In its simplest form, its equivalent to:-
 *		`manager->sharedMemoryFile_mmap_base + offset`
 *		and check if offset is not out of range of current mapped memory
 *		and if offset is out of range, call expandSharedMemory().
 *
 *	The inner set of ternary operators check if for accessing
 *	specified offset, expansion of mapped region is required or no and the outer set of ternary
 *	operator computes the address at the specified offset and in case if expandSharedMemory()
 *	was called and got failed, it returns NULL.
 *
 *	Extra `sizeof(CNode) + sizeof(INode)` are added because doing so saves a lot more
 *	additional checks. In general GOTO_OFFSET is used in __dtsharedmemory_insert()
 *	and __dtsharedmemory_search(),
 *	and is majorly useful when offsets `traverser` and `currentINode->mainNode` need to be
 *	calculated. When traverser's value is fetched, obviously, the next step would be:
 *	`currentINode = manager->baseAddressOfSharedMemory + traverser`
 *	After which `currentINode->mainNode` will be accessed.
 *	When value of traverser is fetched, it only ensures that upto traverser,
 *	memory is within range and expansion is not needed. There can be a case
 *	when till reaching the offset `traverser`, no expansion was required but
 *	when `currentINode->mainNode` is accessed it actually needs availability upto
 *	`traverser + sizeof(INode)` else `currentINode->mainNode` will crash.
 *	Similarly when `currentCNode->possibilities[x]` is accessed, the offset should be checked
 *	upto `currentINode->mainNode + sizeof(CNode)`.
 *	Hence, to make things simple we always check upto `sizeof(CNode) + sizeof(INode)`.
 **/
#define GOTO_OFFSET(offset)  \
/*Check if expansion needed; evaluates to true if offset can be accessed safely*/ \
(\
/*if*/   ((offset + sizeof(CNode) + sizeof(INode)) > manager->sharedMemoryFile_mapping_size ? \
/*then*/ expandSharedMemory((offset + sizeof(CNode) + sizeof(INode))) :\
/*else*/ true)\
\
/*if expansion was needed, expandSharedMemory() can return true or false*/\
/*if expansion wasn't needed, its anyways is true*/\
\
/*if (above expression evaluates to true)*/?\
\
/*then*/(manager->sharedMemoryFile_mmap_base + offset) :\
/*else*/NULL\
)



/**
 *
 *	The time when an offset to CNode is being accessed, its possible its already
 *	replaced with a new CNode. When DISABLE_DUMPING_AND_RECYCLING is 1,
 *	this wasted offset won't be dumped for reuse and hence can be accessed even after
 *	being replaced. But when it has been dumped, its possible that it gets recycled and
 *	used up before this access was made. This is why after the access we need to ensure
 *	in a loop that parent still points to same child.
 *
 *	Hardcoded names like currentCNode and currentINode will obviously makes usage of
 *	this macro less flexible but probably provides a nice clarity while using this macro.
 *
 **/
#if !(DISABLE_DUMPING_AND_RECYCLING)

/*Empty comments are placed all over to avoid crazy indentation caused by auto-indent on macro definitions*/

#	define GUARD_CNODE_ACCESS(statement)\
/**/	{\
/**/		size_t oldMainNode;\
/**/		do\
/**/		{\
/**/			oldMainNode = currentINode->mainNode;\
/**/			currentCNode = GOTO_OFFSET(oldMainNode);\
/**/			FAIL_IF(!currentCNode, "currentCNode found NULL", false);\
/**/			statement;\
/**/		}while( oldMainNode != currentINode->mainNode );\
/**/	}


#else


#	define GUARD_CNODE_ACCESS(statement)\
/**/	{\
/**/		size_t oldMainNode;\
/**/		oldMainNode = currentINode->mainNode;\
/**/		currentCNode = GOTO_OFFSET(oldMainNode);\
/**/		FAIL_IF(!currentCNode, "currentCNode found NULL", false);\
/**/		statement;\
/**/	}

#endif


//Prototypes #START#
//if vim, press % on '{' to reach end of prototypes
//{

/**
 *
 *	This function allocates memory for a the Global(manager).
 *	It opens status file and shared memory file by calling
 *	openStatusFile() and openSharedMemoryFile().
 *	Those 2 functions set the manager's members to appropriate values.
 *	This function would return if Global(manager) is not NULL.
 *
 *	Arguments:
 *
 *	#Arg1(status_file_name):
 *		Name of the status file.
 *
 *	#Arg2(shared_memory_file_name):
 *		Name of the shared memory file
 *
 **/
bool __dtsharedmemory_set_manager(const char *status_file_name, const char *shared_memory_file_name);



/**
 *
 *	This function opens the file `status_file_name`,
 *	and calls mmap(2) with fd of this file and it sets all status file related members in
 *	Global(manager).
 *
 *	Arguments:
 *
 *	#Arg1(new_manager):
 *		A newly allocated `struct SharedMemoryManager` object.
 *
 *	#Arg2(status_file_name):
 *		Name of the status file.
 *
 **/
bool openStatusFile(struct SharedMemoryManager *new_manager, const char *status_file_name);



/**
 *
 *	This function opens the file `shared_memory_file_name`,
 *	and calls mmap(2) with fd of this file and it sets all shared memory file related members in
 *	Global(manager).
 *
 *	Arguments:
 *
 *	#Arg1(new_manager):
 *		A newly allocated `struct SharedMemoryManager` object.
 *
 *	#Arg2(shared_memory_file_name):
 *		Name of the shared memory file.
 *
 **/
bool openSharedMemoryFile(struct SharedMemoryManager *new_manager, const char *shared_memory_file_name);



/**
 *
 *	This function inserts a string `path` along with `flags` into the shared memory.
 *	If insertion is successful, it returns true else false.
 *	The shared memory follows a ctrie data structure. Although it doesn't implement
 *	tomb nodes due to lack of need to remove nodes.
 *
 *	Arguments:
 *
 *	#Arg1(path):
 *		Path to be inserted into shared memory.
 *
 *	#Arg2(flags):
 *		Tell the characteristics of the path getting inserted.
 *
 **/
bool __dtsharedmemory_insert(const char *path, uint8_t flags);



/**
 *
 *	This function searches for a string `path` in the shared memory.
 *	If found it returns true and sets the value of `flags`. Otherwise,
 *	it returns false.
 *	The shared memory follows a ctrie data structure. Although it doesn't implement
 *	tomb nodes due to lack of need to remove nodes.
 *
 *	Arguments:
 *
 *	#Arg1(path):
 *		Path to be searched in shared memory.
 *
 *	#Arg2(flags):
 *		Tells the characteristics of the path. This argument needs to be passed
 *		by address to get characteristcis associated with the path.
 *
 **/
bool __dtsharedmemory_search(const char *path, uint8_t *flags);



/**
 *
 *  This function is preferred instead of atomic fetch and add because it
 *  lets us make the check if memory limit has reached or if LARGE_MEMORY_NEEDED
 *  is set to 0, it let's us check if limit for that is reached.
 *
 *	Arguments:
 *
 *	#Arg1(bytesToBeReserverd):
 *		As the name and function description above describes, this function
 *		shifts the `writeFromOffset` in status file with an offset `bytesToBeReserverd`.
 *
 *	#Arg2(reservedOffset):
 *		This argument is to be passed by reference by the caller function.
 *		After shifting `writeFromOffset` to a new value, the old value of
 *		`writeFromOffset` belongs to the caller function. The caller function can
 *		write from the reserved offset upto `bytesToBeReserverd`.
 *
 *
 * #### Working of the function ####
 *
 *		This function atomically CASs the value of `writeFromOffset` in status file to
 *		a new value which is given as (`writeFromOffset` + `bytesToBeReserverd`).
 *		Block within range from `reservedOffset` upto `bytesToBeReserved`, after this function
 *		returns, is not a critical section for the caller function and it can write to it
 *		without worrying about thread safety.
 *
 **/
bool reserveSpaceInSharedMemory(size_t bytesToBeReserverd, size_t *reservedOffset);



/**
 *
 *	This function is completely abstracted from main functionality and is
 *	called _only_ through GOTO_OFFSET() macro.
 *
 *	Arguments:
 *
 *	#Arg1(offset):
 *		The offset that triggered need for expansion.
 *
 * #### Working of the function ####
 *
 *		This function updates the Global(manager) to contain updated
 *		mapping to shared memory file and the updated mapping is of new size
 *		`manager->sharedMemoryFile_mapping_size + EXPANDING_SIZE`.
 *
 **/
bool expandSharedMemory(size_t offset);



/**
 *
 *	Returns file size of file given by`name`
 *
 **/
static inline size_t getFileSizeForFile(const char *name)
{
	struct stat fileStats;
	int result;
	fileStats.st_size = -1;
	result = stat(name, &fileStats);
	
	//(size_t)-1 will result in greatest size_t value
	//and even if such a case is encountered in actual without error
	//it anyways will reject any insertions afterwards, so its safe to use
	//largest size_t value as error indicator
	FAIL_IF(result == -1, "stat(2) failed", (size_t)-1);
	
	return fileStats.st_size;
}


/**
 *
 *	Arguments:
 *
 *	#Arg1(copy):
 *		This should be an offset in the shared memory, which was obtained by
 *		reserving sizeof(CNode) bytes by reserveSpaceInSharedMemory().
 *
 *	#Arg2(cNodeToBeCopied):
 *		This is the CNode which needs to be copied. It is passed as a (CNode)
 *		and not as (CNode *) because the data this function needs is static and not dynamic.
 *
 *	#Arg3(index):
 *		This is the new entry to the array which needs to be made in the copy.
 *		This also reserves space for a new child and assigns it to
 *		`copy->possibilities[index]`.
 *		If the value of `index` is negative, "no" updation is made to
 *		`copy->possibilities[index]`.
 *
 *	#Arg4(updated_isEndOfString) and #Arg5(updated_flags):
 *		Function updates `copy->isEndOfString` to `updated_isEndOfString`
 *		and `copy->flags` to `updated_flags`.
 *
 *
 * #### Working of the function ####
 *
 *		This function creates a copy of `cNodeToBeCopied` in `copy`.
 *		If it is given a valid `index` as arg, it reservers memory for a new child
 *		and places it into `copy->possibilities[index]`.
 *		It also updates the `copy->isEndOfString` to `updated_isEndOfString`
 *		and `copy->updated_flags` to `updated_flags`.
 *
 **/
bool createUpdatedCNodeCopy(CNode *copy, CNode cNodeToBeCopied, int index, bool updated_isEndOfString, uint8_t updated_flags);



#if !(DISABLE_DUMPING_AND_RECYCLING)
/**
 *
 * #### Need of this function ####
 *
 *		To update a CNode to contain a new entry in bitmap,
 *		a new copy of the same CNode is created with updated bitmap entry
 *		and placed at a newly reserved offset. This new offset is assigned to
 *		the parent INode by CAS because of which the memory at which old CNode
 *		resides gets wasted. That old offset is dumped into `wastedMemoryDumpYard[]` so that
 *		when writing a new CNode, the wasted offsets can
 *		be reused to write CNode instead of using more memory.
 *
 *	Arguments:
 *
 *	#Arg1(wastedOffset):
 *		This is the offset which was replaced with a new value. So dumping it in
 *		`wastedMemoryDumpYard[]`.
 *
 *	#Arg2(parentINode):
 *		This is the offset to the parent INode of the abandoned CNode offset.
 *		This offset is recorded in order to prevent "common parent problem".
 *
 * #### Common parent race condition problem ####
 *
 *		This is a sick race condition among sibling nodes.
 *		See __dtsharedmemory_insert() to get an idea of the situation.
 *		Suppose 3 threads, each dealing with different child of same parent.
 *		(1st thread)1st one prepared its old offset, and is now preparing new offset to
 *		get ready for CAS. Meanwhile (2nd thread)2nd node CASd the old offset to
 *		point to a new offset and it dumped the old offset for recycling and the
 *		new offset contains updated bitmap. Meanwhile the (3rd thread)3rd node
 *		recycled the offset dumped by the 2nd node and CASd it to be child of current parent.
 *		Bitmap has got updations from 2nd and 3rd child node.
 *		Now when 1st node will attempt to CAS, it will find the oldValue
 *		and the actual value to be equal, because 3rd node reused old offset,
 *		but the new value 1st node replaces don't contain bitmap entries set by
 *		2nd and 3rd node leading to data loss.
 *
 *		""For this reason a node can not recycle offsets dumped by its sibling.""
 *
 *
 * #### Working of the function ####
 *
 *		The function reserves an index in `bitmapForDumping` by CAS in order to indicate,
 *		it is using the index.
 *		It then sets the wasted memory offset at the index in `wastedMemoryDumpYard[]`.
 *		It also sets the parent at the index in `parentINodesOfDumper[]`.
 *		It the sets the `bitmapForRecycling` to indicate this node is ready to be
 *		recycled.
 *
 **/
bool dumpWastedMemory(size_t wastedOffset, size_t parentINode);



/**
 *
 *	Prefer reading this after understanding dumpWastedMemory().
 *
 *	Arguments:
 *
 *	#Arg1(reusableOffset):
 *		This value needs to be passed by reference and it gets assigned
 *		a reusable offset if available in `wastedMemoryDumpYard[]`.
 *
 *	#Arg2(parentINode):
 *		This is the offset to the parent INode of the CNode asking for a reusable
 *		offset. wasted offset can't be recycled if the parent is same/
 *		Check the "common parent race condition problem in comments of dumpWastedMemory()".
 *
 *
 * #### Working of the function ####
 *
 *		The function checks `bitmapForRecycling` bitmap for any set entries.
 *		If any entry is set, it unsets that entry in the `bitmapForRecycling` and CASs the bitmap
 *		to get it updated. It retrieves that value from `wastedMemoryDumpYard[]`,
 *		and then sets the `bitmapForDumping` for that index as true so that
 *		that index becomes free for new wasted offsets to get dumped.
 *
 **/
bool recycleWastedMemory(size_t *reusableOffset, size_t parentINode);
#endif

//}
//Prototypes #END#


bool __dtsharedmemory_set_manager(const char *status_file_name, const char *shared_memory_file_name)
{
	
	//Global(manager) is already set
	if(manager != NULL)
		return true;
	
	
	struct SharedMemoryManager *new_manager = (struct SharedMemoryManager *)malloc(sizeof(struct SharedMemoryManager));
	
	FAIL_IF(new_manager == NULL, "malloc(2) failed", false);
	
	bool result;
	
	result = openStatusFile(new_manager, status_file_name);
	
	if (!result)
	{
		free(new_manager);
		print_error("openStatusFile() failed");
		return false;
	}
	
	result = openSharedMemoryFile(new_manager, shared_memory_file_name);
	
	if (!result)
	{
		free(new_manager);
		print_error("openSharedMemoryFile() failed");
		return false;
	}
	
	struct SharedMemoryManager *old_manager;
	
	do
	{
		old_manager = manager;

		if(old_manager != NULL)
		//Global(manager) was set by some other thread
		{
			munmap(new_manager->sharedMemoryFile_mmap_base, new_manager->sharedMemoryFile_mapping_size);
			munmap(new_manager->statusFile_mmap_base, sizeof(struct SharedMemoryStatus));
			free(new_manager);
			
			return true;
		}
		
	} while ( !CAS_ptr( &old_manager, new_manager, &(manager) ) );
	
	return true;
	
}



bool openStatusFile(struct SharedMemoryManager *new_manager, const char *status_file_name)
{
	
	FAIL_IF(new_manager == NULL, "Arg(new_manager) is NULL", false);
	FAIL_IF(status_file_name == NULL || *status_file_name == '\0', "Invalid name for status file", false);
	
	int result;
	bool is_truncate_needed;
	size_t stat_check;
	
	struct{
		
		const char *name;
		int fd;
		size_t size;
		
	}statusFile;
	
	statusFile.name	= new_manager->statusFile_name = status_file_name;
	statusFile.size = sizeof(struct SharedMemoryStatus);
	statusFile.fd   = open(statusFile.name, O_RDWR, FILE_PERMISSIONS);
	
	
	if(statusFile.fd >= 0)
	{
		//Most frequent case, so kept on top
		//Yea, it does nothing
	}
	else if(statusFile.fd == -1 && (access(statusFile.name, F_OK) == -1))
	//File doesn't exist
	{
		//Create file
		//Doesn't overwrite existing file because O_EXCL is added in flags.
		statusFile.fd = open(statusFile.name, O_CREAT | O_EXCL | O_TRUNC | O_RDWR, FILE_PERMISSIONS);
		
		if(statusFile.fd == -1)
		{
			//Some other thread or process may have entered the code and created the file
			//before this thread could. Assuming that, attempt to open the file again.
			
			statusFile.fd = open(statusFile.name, O_RDWR, FILE_PERMISSIONS);
			FAIL_IF(statusFile.fd == -1, "open(2) failed", false);
		}
	}
	else if(statusFile.fd == -1)
	{
		/*
		 *Its is possible that file was created just before access(2) was called in
		 *the above `if` condition so trying to open again
		 */
		statusFile.fd = open(statusFile.name, O_RDWR, FILE_PERMISSIONS);
		FAIL_IF(statusFile.fd == -1, "open(2) failed", false);
		
	}
	else
	{
		print_error("Unknown error! File descriptor negative but not -1");
		return false;
	}
	
	
	if (statusFile.fd >= 0)
	{
		
		is_truncate_needed = ((stat_check = getFileSizeForFile(statusFile.name)) == 0) ? true : false;
		
		FAIL_IF(stat_check == (size_t)-1, "getFileSizeForFile() failed", false);
		
		if (is_truncate_needed)
		{
			result = truncate(statusFile.name, statusFile.size);
			FAIL_IF(result == -1, "truncate(2) failed", false);
		}
		
		
		new_manager->statusFile_mmap_base =
		mmap(NULL, statusFile.size, PROT_READ | PROT_WRITE, MAP_SHARED, statusFile.fd, 0);
		
		FAIL_IF(new_manager->sharedMemoryFile_mmap_base == MAP_FAILED, "mmap(2) failed", false);
		
		new_manager->statusFile_fd = statusFile.fd;
		
	}
	
	
	/*
	 *	As the file size is expanded by truncate(2) which fills the file with '\0',
	 *	sharedMemoryFileSize and writeFromOffset are initially 0.
	 *
	 *	Whichever thread or process opens the status file, needs to check for
	 *	new_manager->statusFile_mmap_base->sharedMemoryFileSize and
	 *	new_manager->statusFile_mmap_base->writeFromOffset.
	 *	If these two are found 0, replace them with appropriate initial values.
	 */
	size_t oldValue, newValue;
	
	oldValue = 0;
	newValue = INITIAL_FILE_SIZE;
	
	//Should always be more than at least ROOT_SIZE
	FAIL_IF(newValue < ROOT_SIZE, "INITIAL_FILE_SIZE is too less", false);
	
	
	result =
	CAS_size_t(&oldValue, newValue, &(new_manager->statusFile_mmap_base->sharedMemoryFileSize) );
	
	
	FAIL_IF(new_manager->statusFile_mmap_base->sharedMemoryFileSize == 0, "CAS for sharedMemoryFileSize failed", false);
	
	oldValue = 0;
	newValue = 0 + ROOT_SIZE; //write after ROOT_SIZE bytes
	
	result =
	CAS_size_t(&oldValue, newValue, &(new_manager->statusFile_mmap_base->writeFromOffset) );
	
	
	FAIL_IF(new_manager->statusFile_mmap_base->writeFromOffset == 0, "CAS for writeFromOffset failed", false);
	
	
	
	return true;
	
}



bool openSharedMemoryFile(struct SharedMemoryManager *new_manager, const char *shared_memory_file_name)
{
	
	FAIL_IF(new_manager == NULL, "Arg(new_manager) is NULL", false);
	FAIL_IF(shared_memory_file_name == NULL || *shared_memory_file_name == '\0', "Invalid name for shared memory file", false);
	
	bool is_truncate_needed;
	size_t stat_check;
	int result;
	
	struct{
		
		const char *name;
		int fd;
		size_t size;
		
	}sharedMemoryFile;
	
	sharedMemoryFile.name = new_manager->sharedMemoryFile_name = shared_memory_file_name;
	sharedMemoryFile.size = new_manager->statusFile_mmap_base->sharedMemoryFileSize;
	sharedMemoryFile.fd   = open(sharedMemoryFile.name, O_RDWR, FILE_PERMISSIONS);
	
	
	if(sharedMemoryFile.fd >= 0)
	{
		//Most frequent case, so kept on top
		//Yea, it does nothing
	}
	else if(sharedMemoryFile.fd == -1 && (access(sharedMemoryFile.name, F_OK) == -1))
	//File doesn't exist
	{
		//Create file
		//Doesn't overwrite existing file because O_EXCL is added in flags.
		sharedMemoryFile.fd = open(sharedMemoryFile.name, O_CREAT | O_EXCL | O_TRUNC | O_RDWR, FILE_PERMISSIONS);
		
		if(sharedMemoryFile.fd == -1)
		{
			//Some other thread or process may have entered the code and created the file
			//before this thread could. Assuming that, attempt to open the file again.
			
			sharedMemoryFile.fd = open(sharedMemoryFile.name, O_RDWR, FILE_PERMISSIONS);
			FAIL_IF(sharedMemoryFile.fd == -1, "open(2) failed", false);
		}
	}
	else if(sharedMemoryFile.fd == -1)
	{
		/*
		 *Its is possible that file was created just before access(2) was called in
		 *the above `if` condition so trying to open again
		 */
		sharedMemoryFile.fd = open(sharedMemoryFile.name, O_RDWR, FILE_PERMISSIONS);
		FAIL_IF(sharedMemoryFile.fd == -1, "open(2) failed", false);
		
	}
	else
	{
		print_error("Unknown error! File descriptor negative but not -1");
		return false;
	}
	
	
	
	if (sharedMemoryFile.fd >= 0)
	{
		
		is_truncate_needed = ((stat_check = getFileSizeForFile(sharedMemoryFile.name)) == 0) ? true : false;
		
		FAIL_IF(stat_check == (size_t)-1, "getFileSizeForFile() failed", false);
		
		if (is_truncate_needed)
		{
			result = truncate(sharedMemoryFile.name, sharedMemoryFile.size);
			FAIL_IF(result == -1, "truncate(2) failed", false);
		}
		
		new_manager->sharedMemoryFile_mmap_base = mmap(NULL, sharedMemoryFile.size, PROT_READ | PROT_WRITE, MAP_SHARED, sharedMemoryFile.fd, 0);
		
		FAIL_IF(new_manager->sharedMemoryFile_mmap_base == MAP_FAILED, "mmap(2) failed", false);
		
		new_manager->sharedMemoryFile_mapping_size = sharedMemoryFile.size;
		
		new_manager->sharedMemoryFile_fd = sharedMemoryFile.fd;
		
	}
	
	
	
	//	If rootINode->mainNode is 0, need to initialise it.
	//	It will be 0 the first time of this CAS because file is given its size
	//	through truncate(2), which fills file with '\0'.
	//	Every thread opening this file needs to make this check.
	
	size_t oldValue, newValue;
	
	oldValue = 0;
	newValue = sizeof(INode);
	
	INode *rootINode = new_manager->sharedMemoryFile_mmap_base;
	
	CAS_size_t(&oldValue, newValue, &(rootINode->mainNode));
	
	FAIL_IF(rootINode->mainNode == 0, "Couldn't set root INode's mainNode", false);
	
	return true;
	
}



bool __dtsharedmemory_insert(const char *path, uint8_t flags)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	FAIL_IF(path == NULL, "Arg(path) is NULL", false);
	
	size_t traverser = 0;
	size_t oldValue, newValue;
	
	INode *currentINode;
	CNode *currentCNode;
	CNode *copiedCNode;
	CNode tempCNode;
	
	size_t offsetToCopiedChildCNode;
	
	int currentCharacter;
	
	uint8_t pathCharacter;
	
	bool result;
	bool entryFor_pathCharacter_alreadyExists ;
	bool entryFor_pathCharacter;
	bool isEndOfString;
	
	for (currentCharacter = 0 ; *(path + currentCharacter) != '\0' ; ++currentCharacter)
	{
		
		pathCharacter = *(path + currentCharacter);
		
#if (LOWER_LIMIT > 13)
		//13 ascii is custom icon representer on macOS
		if (pathCharacter == 13)
			continue;
#endif
		
		FAIL_IF(pathCharacter > (uint8_t)UPPER_LIMIT, "Not accepting characters above UPPER_LIMIT", false);
		FAIL_IF(pathCharacter < (uint8_t)LOWER_LIMIT, "Not accepting characters below LOWER_LIMIT", false);
		
		currentINode = GOTO_OFFSET(traverser);
		
		FAIL_IF(!currentINode, "currentINode found NULL", false);
		
		
		GUARD_CNODE_ACCESS
		(
		entryFor_pathCharacter = (bool)currentCNode->possibilities[pathCharacter - LOWER_LIMIT];
		)
		
		
		if ( entryFor_pathCharacter == false )
		{
			
			/**
			 *	Entering this `if` block means the node doesn't contain pathCharacter
			 *	This block would create a copy of currentCNode with updated values
			 *	and try repeated CAS on currentINode->mainNode.
			 **/
			
			
#if !(DISABLE_DUMPING_AND_RECYCLING)
			
			result = recycleWastedMemory(&offsetToCopiedChildCNode, traverser);
			
			if (!result)
			{
				result = reserveSpaceInSharedMemory(sizeof(CNode), &offsetToCopiedChildCNode);
			}
			
#else
			
			result = reserveSpaceInSharedMemory(sizeof(CNode), &offsetToCopiedChildCNode);
			
#endif
			
			FAIL_IF(!result, "Failed to insert new node", false);
			
			copiedCNode = GOTO_OFFSET(offsetToCopiedChildCNode);
			
			FAIL_IF(!copiedCNode, "copiedCNode found NULL", false);
			
			entryFor_pathCharacter_alreadyExists = false;
			
			do
			{
				
				oldValue = currentINode->mainNode;
				
				GUARD_CNODE_ACCESS
				(
				entryFor_pathCharacter = currentCNode->possibilities[pathCharacter - LOWER_LIMIT];
				)
				
				if ( entryFor_pathCharacter )
				{
					//Some other thread may create a copy of the currentCNode and
					//try to update the same array entry as this thread is doing.
					//If this check is not made, one of the 2 insertions will fail.
					
#if !(DISABLE_DUMPING_AND_RECYCLING)
					//As the newly reserved offset is wasted, dump it.
					dumpWastedMemory(offsetToCopiedChildCNode, traverser);
#endif
					entryFor_pathCharacter_alreadyExists = true;
					break;
					
				}
				
				newValue = offsetToCopiedChildCNode;
				
				//Need not be inside GUARD_CNODE_ACCESS because if the CNode changes,
				//cas will fail anyway
				tempCNode = *currentCNode;
				
				result = createUpdatedCNodeCopy(copiedCNode, tempCNode, pathCharacter - LOWER_LIMIT, tempCNode.isEndOfString, tempCNode.flags);
				
				FAIL_IF(!result, "Failed to update CNode", false);
				
				
			} while ( !CAS_size_t( &oldValue, newValue, &(currentINode->mainNode) ) );
			
			
			if (!entryFor_pathCharacter_alreadyExists)
			{
				
#if !(DISABLE_DUMPING_AND_RECYCLING)
				dumpWastedMemory(oldValue, traverser);
#endif
				currentCNode = copiedCNode;
				
			}
			
		}
		
		GUARD_CNODE_ACCESS
		(
		traverser = currentCNode->possibilities[pathCharacter - LOWER_LIMIT];
		)
		
	}
	
	/**
	 *	If the path getting inserted is a prefix, and the last character of
	 *	the inputted path is '/', just ignore the '/' and treat it like it was
	 *	never in the Arg(path) because that's how __dtsharedmemory_search() will
	 *	detect the prefix. The code in the end of the func  makes the ending node of a prefix path
	 *	contain 0 in the `possibilities` array for '/' so that search can detect the prefix.
	 **/
	if ((pathCharacter == '/' && (flags & IS_PREFIX))     ==    false)
	{
		currentINode = GOTO_OFFSET(traverser);
		FAIL_IF(!currentINode, "currentINode found NULL", false);
	}
	
	
#if !(DISABLE_DUMPING_AND_RECYCLING)
	
	result = recycleWastedMemory(&offsetToCopiedChildCNode, traverser);
	
	if (!result)
	{
		result = reserveSpaceInSharedMemory(sizeof(CNode), &offsetToCopiedChildCNode);
	}
	
#else
	
	result = reserveSpaceInSharedMemory(sizeof(CNode), &offsetToCopiedChildCNode);
	
#endif
	
	FAIL_IF(!result, "Failed to insert new node", false);
	
	copiedCNode = GOTO_OFFSET(offsetToCopiedChildCNode);
	
	FAIL_IF(!copiedCNode, "copiedCNode found NULL", false);
	
	do
	{
		
		oldValue = currentINode->mainNode;
		newValue = offsetToCopiedChildCNode;
		
		//Need not be inside GUARD_CNODE_ACCESS because if the CNode changes,
		//CAS will fail anyway
		tempCNode = *currentCNode;
		
		//-1 in the function call below indicates no updations required in array.
		//Only need to change isEndOfString and flags.
		isEndOfString = true;
		result = createUpdatedCNodeCopy(copiedCNode, tempCNode, -1, isEndOfString, flags);
		
		FAIL_IF(!result, "Failed to update CNode", false);
		
		if (flags & IS_PREFIX)
		{
			//So that paths that have already been inseted with this
			//prefix can be searched as prefixes now (see __dtsharedmemory_search())
			copiedCNode->possibilities['/' - LOWER_LIMIT] = 0;
		}
		
	} while ( !CAS_size_t( &oldValue, newValue, &(currentINode->mainNode) ) );
	
#if !(DISABLE_DUMPING_AND_RECYCLING)
	dumpWastedMemory(oldValue, traverser);
#endif
	
	return true;
}



bool __dtsharedmemory_search(const char *path, uint8_t *flags)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	FAIL_IF(path == NULL, "Arg(path) is NULL", false);
	
	
	int currentCharacter;
	uint8_t pathCharacter;
	
	
	size_t traverser = 0;
	INode *currentINode;
	CNode *currentCNode;
	bool isEndOfString;
	
	bool entryFor_pathCharacter;
	uint8_t flagsForCurrentCNode;
	
	for (currentCharacter = 0 ; *(path + currentCharacter) != '\0' ; ++currentCharacter)
	{
		
		pathCharacter = *(path + currentCharacter);
		
#if (LOWER_LIMIT > 13)
		//13 ascii is custom icon representer
		if (pathCharacter == 13)
			continue;
#endif
		
		FAIL_IF(pathCharacter > (uint8_t)UPPER_LIMIT, "Not accepting characters above UPPER_LIMIT", false);
		FAIL_IF(pathCharacter < (uint8_t)LOWER_LIMIT, "Not accepting characters below LOWER_LIMIT", false);
		
		
		currentINode = GOTO_OFFSET(traverser);
		
		FAIL_IF(!currentINode, "currentINode found NULL", false);
		
		GUARD_CNODE_ACCESS
		(
		entryFor_pathCharacter = currentCNode->possibilities[pathCharacter - LOWER_LIMIT];
		flagsForCurrentCNode   = currentCNode->flags;
		)
		
		if ( entryFor_pathCharacter == false)
		{
			
			if (pathCharacter == '/' && (flagsForCurrentCNode & IS_PREFIX))
			{
				*flags = flagsForCurrentCNode;
				return true;
			}
			
			
			//Doesn't exist in shared memory
			return false;
		}
		
		
		GUARD_CNODE_ACCESS
		(
		traverser = currentCNode->possibilities[pathCharacter - LOWER_LIMIT];
		)
		
	}
	
	currentINode = GOTO_OFFSET(traverser);
	
	FAIL_IF(!currentINode, "currentINode found NULL", false);
	
	GUARD_CNODE_ACCESS
	(
	*flags        = currentCNode->flags;
	isEndOfString = currentCNode->isEndOfString;
	)
	
	return isEndOfString;
}



bool reserveSpaceInSharedMemory(size_t bytesToBeReserverd, size_t *reservedOffset)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	
	size_t oldValue, newValue;
	
	do
	{
		oldValue = manager->statusFile_mmap_base->writeFromOffset;
		
		//PADDING_BYTES are added so that recycled offsets can use an odd offset
		//by adding 1. PADDING_BYTES create extra space so 1 can be added to recycled offsets.
		newValue = oldValue + bytesToBeReserverd + PADDING_BYTES;
		
#if !(LARGE_MEMORY_NEEDED)
		FAIL_IF(newValue >= UINT32_MAX, "Set LARGE_MEMORY_NEEDED to 1 in dtsharedmemory.h to use more memory", false);
#endif
		
		FAIL_IF(newValue <= oldValue, "Memory limit reached", false);
		
	} while ( !CAS_size_t( &oldValue, newValue, &(manager->statusFile_mmap_base->writeFromOffset) ) );
	
	*reservedOffset = oldValue;
	
	return true;
	
}



bool expandSharedMemory(size_t offset)
{
	
#if DISABLE_MEMORY_EXPANSION && 1
	
	FAIL_IF(true, "Memory expansion is disabled", false);
	
#endif
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	
	size_t newSize;
	int result;
	struct SharedMemoryManager *old_manager, *new_manager;
	
	
	/**
	 *	Below we are just choosing the largest known file size
	 *	Any number of cases can arise that may prevent the code from
	 *	adding EXPANDING_SIZE to a much old known file size but the next 2 checks
	 *	make it very very rare making such a condition only theoritical and EXPANDING_SIZE
	 *	is big enough to even survive that in most cases.
	 *	To explain it further, sometimes a particular processes may have a
	 *	very old `sharedMemoryFile_mapping_size` whereas other processes may have went
	 *	way far, that's why `statusFile_mmap_base->sharedMemoryFileSize` is also checked.
	 **/
	
	newSize = manager->statusFile_mmap_base->sharedMemoryFileSize;
	
	newSize = manager->sharedMemoryFile_mapping_size > newSize ? manager->sharedMemoryFile_mapping_size : newSize;
	
	
	newSize += EXPANDING_SIZE;
	
	//Some other thread already expanded memory
	if(manager->sharedMemoryFile_mapping_size > offset)
		return true;
	//Better if we avoid expanding if other thread already fulfilled requirement
	
	
	//File size expansion
	if (manager->statusFile_mmap_base->sharedMemoryFileSize < newSize)
	{
		//using truncate(2) instead of ftruncate(2)
		//seems safer because of unpredicted fd clashes with
		//processes in which library is injected
		result = truncate(manager->sharedMemoryFile_name, newSize);
		FAIL_IF(result == -1, "truncate(2) failed", false);
	}
	
	
	//Make new manager with updated mappings to replace the current Global(manager)
	new_manager = (struct SharedMemoryManager *)malloc(sizeof(struct SharedMemoryManager));
	FAIL_IF(new_manager == NULL, "malloc(2) failed", false);
	
	memcpy(new_manager, manager, sizeof(struct SharedMemoryManager));
	
	new_manager->sharedMemoryFile_mmap_base = mmap(NULL, newSize, PROT_READ | PROT_WRITE, MAP_SHARED, manager->sharedMemoryFile_fd, 0 );
	FAIL_IF(new_manager->sharedMemoryFile_mmap_base == MAP_FAILED, "mmap(2) failed", false);
	new_manager->sharedMemoryFile_mapping_size = newSize;
	
	
	//Replace Global(manager)
	do
	{
		
		old_manager = manager;
		
		if(old_manager->sharedMemoryFile_mapping_size >= new_manager->sharedMemoryFile_mapping_size)
		{
			munmap(new_manager->sharedMemoryFile_mmap_base, new_manager->sharedMemoryFile_mapping_size);
			free(new_manager);
			
			return true;
		}
		
	} while ( !CAS_ptr( &old_manager, new_manager, &(manager) ) );
	
	
	
	//Update file size in status file
	size_t oldValue, newValue;
	do
	{
		oldValue = manager->statusFile_mmap_base->sharedMemoryFileSize;
		newValue = newSize;
		
		if (newValue <= oldValue)
		{
			break;
		}
		
	} while ( !CAS_size_t( &oldValue, newValue, &(manager->statusFile_mmap_base->sharedMemoryFileSize) ) );
	
	
	return true;
	
}



bool createUpdatedCNodeCopy(CNode *copy, CNode cNodeToBeCopied, int index, bool updated_isEndOfString, uint8_t updated_flags)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	
	
	//copying the complete old CNode
	copy = memcpy(copy, &cNodeToBeCopied, sizeof(CNode));
	
	
	//Only create a new entry if `index` is non negative.
	if (index >= 0)
	{
		bool result;
		size_t bytesToBeReserverd, writeFromOffset;
		
		//Making changes to the `copy` by adding new child
		
		bytesToBeReserverd = sizeof(INode) + sizeof(CNode);
		
		result = reserveSpaceInSharedMemory(bytesToBeReserverd, &writeFromOffset);
		
		FAIL_IF(!result, "Failed to create an updated copy of CNode", false);
		
		
		//Creating child INode
		
		INode *baseAddressOfINode = GOTO_OFFSET(writeFromOffset);
		FAIL_IF(!baseAddressOfINode, "baseAddressOfINode found NULL", false);
		//	The values of child CNode are to be set to 0. Because truncate(2) already
		//	fills the file with '\0', this eliminates the need to do this ourselves.
		baseAddressOfINode->mainNode = writeFromOffset + sizeof(INode);
		
		copy->possibilities[index] = writeFromOffset;
	}
	
	
	copy->isEndOfString = updated_isEndOfString;
	copy->flags         = updated_flags;
	
	return true;
	
}



int __dtsharedmemory_getStatusFileFd()
{
	return (manager != NULL ? manager->statusFile_fd : -1);
}

int __dtsharedmemory_getSharedMemoryFileFd()
{
	return (manager != NULL ? manager->sharedMemoryFile_fd : -1);
}

size_t __dtsharedmemory_getUsedSharedMemorySize()
{
	return (manager != NULL ? manager->statusFile_mmap_base->writeFromOffset : 0);
}

bool __dtsharedmemory_reset_fd()
{
	
	//returns true because because no fd exists
	if (manager == NULL)
		return true;
	
	struct SharedMemoryManager *old_manager, *new_manager;
	
	new_manager = (struct SharedMemoryManager *)malloc(sizeof(struct SharedMemoryManager));
	
	FAIL_IF(new_manager == NULL, "malloc(2) failed", false);
	
	int statusFile_fd, sharedMemoryFile_fd;
	
	sharedMemoryFile_fd = open(manager->sharedMemoryFile_name, O_RDWR, FILE_PERMISSIONS);
	
	FAIL_IF(sharedMemoryFile_fd == -1, "open(2) failed", false);
	
	statusFile_fd = open(manager->statusFile_name, O_RDWR, FILE_PERMISSIONS);
	
	FAIL_IF(statusFile_fd == -1, "open(2) failed", false);
	
	do
	{
		old_manager = manager;
		memcpy(new_manager, old_manager, sizeof(struct SharedMemoryManager));
		
		new_manager->sharedMemoryFile_fd = sharedMemoryFile_fd;
		new_manager->statusFile_fd       = statusFile_fd;
		
	} while ( !CAS_ptr( &old_manager, new_manager, &(manager) ) );
	
	return true;
}


#if !(DISABLE_DUMPING_AND_RECYCLING)

bool dumpWastedMemory(size_t wastedOffset, size_t parentINode)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	
	int i;
	size_t oldBitmap, newBitmap;
	int bitmapIndex, bitmapOffset;
	bool isFull;
	
	++ wastedOffset;
	/**
	 *	To make it go to an odd offset
	 *	Its a unique property of recycled offsets that they are always odd.
	 *	second time, it gets even and is rejected by the next `if` statement.
	 *
	 *	This unique property of making recycled offsets 1 is needed in order
	 *	to detect if same offset is getting dumped the second time.
	 *	Same offset should not get dumped a second time because it makes it loose
	 *	its previous parent INode data and that can lead to "common parent problem".
	 **/
	if (wastedOffset % 2 == 0)
	{
		return false;
	}
	
	
	do
	{
		
		isFull = true;
		
		for (i = 0 ; i < DUMP_YARD_SIZE ; ++i)
		{
			
			bitmapIndex  = i / NO_OF_BITS;
			bitmapOffset = i % NO_OF_BITS;
			
			oldBitmap = manager->statusFile_mmap_base->bitmapForDumping[bitmapIndex];
			
			if (getBitmapAtIndex(oldBitmap, bitmapOffset) == false)
			{
				newBitmap = setBitmapAtIndex(oldBitmap, bitmapOffset);
				isFull    = false;
				break;
			}
		}
		
		//Wasted memory dump yard full
		if (isFull)
			return false;
		
	} while( !CAS_size_t( &oldBitmap, newBitmap, &(manager->statusFile_mmap_base->bitmapForDumping[bitmapIndex]) ) );
	
	
	manager->statusFile_mmap_base->wastedMemoryDumpYard[i] = wastedOffset;
	manager->statusFile_mmap_base->parentINodesOfDumper[i] = parentINode;
	
	
	do
	{
		oldBitmap = manager->statusFile_mmap_base->bitmapForRecycling[bitmapIndex];
		newBitmap = setBitmapAtIndex(oldBitmap, bitmapOffset);
		
	} while ( !CAS_size_t( &oldBitmap, newBitmap, &(manager->statusFile_mmap_base->bitmapForRecycling[bitmapIndex]) ) );
	
	return true;
	
}



bool recycleWastedMemory(size_t *reusableOffset, size_t parentINode)
{
	
	FAIL_IF(manager == NULL, "Global(manager) is NULL", false);
	
	int i;
	size_t oldBitmap, newBitmap, parentINodeOfDumper;
	int bitmapIndex, bitmapOffset;
	bool isEmpty = true;
	
	
	do
	{
		isEmpty = true;
		
		for (i = 0 ; i < DUMP_YARD_SIZE ; ++i)
		{
			
			bitmapIndex  = i / NO_OF_BITS;
			bitmapOffset = i % NO_OF_BITS;
			
			oldBitmap = manager->statusFile_mmap_base->bitmapForRecycling[bitmapIndex];
			
			if (getBitmapAtIndex(oldBitmap, bitmapOffset) == true)
			{
				newBitmap = unsetBitmapAtIndex(oldBitmap, bitmapOffset);
				isEmpty   = false;
				break;
			}
		}
		
		//Wasted memory dump yard empty
		if (isEmpty)
			return false;
		
	} while( !CAS_size_t( &oldBitmap, newBitmap, &(manager->statusFile_mmap_base->bitmapForRecycling[bitmapIndex]) ) );
	
	*reusableOffset	    = manager->statusFile_mmap_base->wastedMemoryDumpYard[i];
	parentINodeOfDumper = manager->statusFile_mmap_base->parentINodesOfDumper[i];
	manager->statusFile_mmap_base->wastedMemoryDumpYard[i] = 0;
	
	do
	{
		oldBitmap = manager->statusFile_mmap_base->bitmapForDumping[bitmapIndex];
		newBitmap = unsetBitmapAtIndex(oldBitmap, bitmapOffset);
		
	} while ( !CAS_size_t( &oldBitmap, newBitmap, &(manager->statusFile_mmap_base->bitmapForDumping[bitmapIndex]) ) );
	
	if (parentINode == parentINodeOfDumper)
	{
		dumpWastedMemory(*reusableOffset - 1, parentINode);
		return false;
	}
	
	return true;
	
}

#endif
