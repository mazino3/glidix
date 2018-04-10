/*
	Glidix kernel

	Copyright (c) 2014-2017, Madd Games.
	All rights reserved.
	
	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	
	* Redistributions of source code must retain the above copyright notice, this
	  list of conditions and the following disclaimer.
	
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	
	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
	OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <glidix/util/common.h>
#include <glidix/storage/storage.h>
#include <glidix/util/memory.h>
#include <glidix/util/string.h>
#include <glidix/display/console.h>
#include <glidix/hw/physmem.h>

/**
 * Bitmap of used drive letters (for /dev/sdX). Bit n represents letter 'a'+n,
 * for example bit 0 represents 'a', bit 1 represents 'b', etc. Note that there
 * are only 26 possible letters.
 */
static uint32_t sdLetters;

/**
 * Maps drive letters (index 0 being 'a', index 1 being 'b', etc) to respective storage devices.
 */
Mutex mtxList;
static StorageDevice* sdList[26];

static void reloadPartTable(StorageDevice *sd);

static char sdAllocLetter()
{
	int i;
	for (i=0; i<26; i++)
	{
		if (atomic_test_and_set(&sdLetters, i) == 0)
		{
			return 'a'+i;
		};
	};
	
	return 0;
};

static void sdDownref(StorageDevice *sd)
{
	if (__sync_add_and_fetch(&sd->refcount, -1) == 0)
	{
		while (sd->cmdq != NULL)
		{
			SDCommand *cmd = sd->cmdq;
			sd->cmdq = cmd->next;
			kfree(cmd);
		};
		
		kfree(sd);
	};
};

static void sdUpref(StorageDevice *sd)
{
	__sync_fetch_and_add(&sd->refcount, 1);
};

static void sdFreeLetter(char c)
{
	uint32_t mask = ~(1 << (c-'a'));
	__sync_fetch_and_and(&sdLetters, mask);
};

static void sdPush(StorageDevice *sd, SDCommand *cmd)
{
	// sd must already be locked!
	cmd->next = NULL;
	if (sd->cmdq == NULL)
	{
		sd->cmdq = cmd;
	}
	else
	{
		SDCommand *last = sd->cmdq;
		while (last->next != NULL) last = last->next;
		last->next = cmd;
	};
	
	semSignal(&sd->semCommands);
};

void sdInit()
{
	sdLetters = 0;
	memset(sdList, 0, sizeof(void*)*26);
	mutexInit(&mtxList);
};

static int sdfile_ioctl(Inode *inode, File *fp, uint64_t cmd, void *params)
{
	if (cmd == IOCTL_SDI_IDENTITY)
	{
		SDHandle *data = (SDHandle*) fp->filedata;
		mutexLock(&data->sd->lock);
		SDParams *sdpars = (SDParams*) params;
		sdpars->flags = data->sd->flags;
		sdpars->blockSize = data->sd->blockSize;
		sdpars->totalSize = data->size;
		mutexUnlock(&data->sd->lock);
		return 0;
	}
	else if (cmd == IOCTL_SDI_EJECT)
	{
		SDHandle *data = (SDHandle*) fp->filedata;
		if (data->sd->totalSize != 0)
		{
			// non-removable
			return -1;
		};
		
		Semaphore lock;
		semInit2(&lock, 0);

		SDCommand *cmd = (SDCommand*) kmalloc(sizeof(SDCommand));
		cmd->type = SD_CMD_EJECT;
		cmd->block = NULL;
		cmd->cmdlock = &lock;
		cmd->flags = 0;

		sdPush(data->sd, cmd);
		semWait(&lock);				// wait for the eject operation to finish
	
		// (cmd was freed by the driver)
		return 0;
	};

	ERRNO = ENODEV;
	return -1;
};

static void sdFlushTree(StorageDevice *sd, BlockTreeNode *node, int level, uint64_t pos)
{
	uint64_t i;
	for (i=0; i<128; i++)
	{
		if (node->entries[i] & SD_BLOCK_DIRTY)
		{
			node->entries[i] &= ~SD_BLOCK_DIRTY;
			if (level == 6)
			{
				uint64_t canaddr = (node->entries[i] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
				uint64_t *pte = (uint64_t*) ((canaddr >> 9) | 0xffffff8000000000L);
				uint64_t phys = (*pte) & 0x0000fffffffff000L;
				
				Semaphore semCmd;
				semInit2(&semCmd, 0);
				
				SDCommand *cmd = NEW(SDCommand);
				cmd->type = SD_CMD_WRITE_TRACK;
				cmd->block = (void*) phys;
				cmd->pos = ((pos << 7) | i) << 15;
				cmd->cmdlock = &semCmd;
				cmd->status = NULL;
				cmd->flags = 0;
				sdPush(sd, cmd);
				
				semWait(&semCmd);
			}
			else
			{
				uint64_t canaddr = (node->entries[i] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
				sdFlushTree(sd, (BlockTreeNode*)canaddr, level+1, (pos << 7) | i);
			};
		};
	};
};

static void sdFlush(StorageDevice *sd)
{
	// call this only when the cache is locked
	sdFlushTree(sd, &sd->cacheTop, 0, 0);
};

static int sdfile_flush(Inode *inode)
{
	SDDeviceFile *fdev = (SDDeviceFile*) inode->fsdata;
	mutexLock(&fdev->sd->cacheLock);
	sdFlush(fdev->sd);
	mutexUnlock(&fdev->sd->cacheLock);
	
	return 0;
};

static void sdfile_close(Inode *inode, void *filedata)
{
	sdfile_flush(inode);
	
	SDHandle *handle = (SDHandle*) filedata;
	mutexLock(&handle->sd->lock);
	
	if (handle->partIndex == -1)
	{
		handle->sd->openParts = 0;
		mutexUnlock(&handle->sd->lock);
		reloadPartTable(handle->sd);
	}
	else
	{
		uint64_t mask = 1UL << (uint64_t)handle->partIndex;
		handle->sd->openParts &= ~mask;
		mutexUnlock(&handle->sd->lock);
	};

	sdDownref(handle->sd);
	kfree(handle);
};

static ssize_t sdRead(StorageDevice *sd, uint64_t pos, void *buf, size_t size)
{
	if (sd->flags & SD_HANGUP)
	{
		ERRNO = ENXIO;
		return -1;
	};
	
	uint8_t *put = (uint8_t*) buf;
	ssize_t sizeRead = 0;
	
	while (size > 0)
	{
		uint64_t offsetIntoPage = pos & (SD_TRACK_SIZE-1);
		uint64_t toRead = SD_TRACK_SIZE - offsetIntoPage;
		
		if (toRead > size)
		{
			toRead = size;
		};
		
		// see if this page is in the cache; otherwise load it
		mutexLock(&sd->cacheLock);
		
		uint64_t i;
		BlockTreeNode *node = &sd->cacheTop;
		for (i=0; i<6; i++)
		{
			uint64_t sub = (pos >> (15 + 7 * (6 - i))) & 0x7F;
			uint64_t entry = node->entries[sub];
			
			if (entry == 0)
			{
				BlockTreeNode *nextNode = NEW(BlockTreeNode);
				memset(nextNode, 0, sizeof(BlockTreeNode));
				
				// bottom 48 bits of address, set usage counter to 1, not dirty
				node->entries[sub] = ((uint64_t) nextNode & 0xFFFFFFFFFFFF) | (1UL << 56);
				
				node = nextNode;
			}
			else
			{
				// increment usage counter
				uint64_t ucnt = entry >> 56;
				if (ucnt != 255)
				{
					node->entries[sub] += (1UL << 56);
				};
				
				// get canonical address
				uint64_t canaddr = (node->entries[sub] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
				
				// follow
				node = (BlockTreeNode*) canaddr;
			};
		};
		
		// see if an entry exists for this track
		uint64_t track = (pos >> 15) & 0x7F;
		uint64_t trackAddr;
		if (node->entries[track] == 0)
		{
			// we need to load the track
			uint64_t physStartFrame = phmAllocFrameEx(8, 0);
			if (physStartFrame == 0)
			{
				mutexUnlock(&sd->cacheLock);
				
				if (sizeRead == 0)
				{
					ERRNO = ENOMEM;
					return -1;
				};
				
				break;
			};
			
			Semaphore semCmd;
			semInit2(&semCmd, 0);
			
			int status = 0;
			
			SDCommand *cmd = NEW(SDCommand);
			cmd->type = SD_CMD_READ_TRACK;
			cmd->block = (void*) (physStartFrame << 12);
			cmd->pos = pos & ~0x7FFFUL;
			cmd->cmdlock = &semCmd;
			cmd->status = &status;
			cmd->flags = 0;
			sdPush(sd, cmd);
			
			semWait(&semCmd);
			
			if (status != 0)
			{
				mutexUnlock(&sd->cacheLock);
				
				if (sizeRead == 0)
				{
					ERRNO = EIO;
					return -1;
				};
				
				break;
			};
			
			void *vptr = mapPhysMemory(physStartFrame << 12, 0x8000);
			node->entries[track] = ((uint64_t) vptr & 0xFFFFFFFFFFFF) | (1UL << 56);
			trackAddr = (uint64_t) vptr;
		}
		else
		{
			trackAddr = (node->entries[track] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
		};
		
		memcpy(put, (void*)(trackAddr + offsetIntoPage), toRead);
		put += toRead;
		sizeRead += toRead;
		pos += toRead;
		size -= toRead;
		
		mutexUnlock(&sd->cacheLock);
	};
	
	return sizeRead;
};

static ssize_t sdWrite(StorageDevice *sd, uint64_t pos, const void *buf, size_t size)
{
	if (sd->flags & SD_HANGUP)
	{
		ERRNO = ENXIO;
		return -1;
	};
	
	const uint8_t *scan = (const uint8_t*) buf;
	ssize_t sizeWritten = 0;
	
	while (size > 0)
	{
		uint64_t offsetIntoPage = pos & (SD_TRACK_SIZE-1UL);
		uint64_t toWrite = SD_TRACK_SIZE - offsetIntoPage;
		
		if (toWrite > size)
		{
			toWrite = size;
		};
		
		// see if this page is in the cache; otherwise load it
		mutexLock(&sd->cacheLock);
		
		uint64_t i;
		BlockTreeNode *node = &sd->cacheTop;
		for (i=0; i<6; i++)
		{
			uint64_t sub = (pos >> (15 + 7 * (6 - i))) & 0x7F;
			uint64_t entry = node->entries[sub];
			
			if (entry == 0)
			{
				BlockTreeNode *nextNode = NEW(BlockTreeNode);
				memset(nextNode, 0, sizeof(BlockTreeNode));
				
				// bottom 48 bits of address, set usage counter to 1, not dirty
				node->entries[sub] = ((uint64_t) nextNode & 0xFFFFFFFFFFFF) | (1UL << 56) | SD_BLOCK_DIRTY;
				
				node = nextNode;
			}
			else
			{
				// increment usage counter
				uint64_t ucnt = entry >> 56;
				if (ucnt != 255)
				{
					node->entries[sub] += (1UL << 56);
				};
				node->entries[sub] |= SD_BLOCK_DIRTY;
				
				// get canonical address
				uint64_t canaddr = (node->entries[sub] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
				
				// follow
				node = (BlockTreeNode*) canaddr;
			};
		};
		
		// see if an entry exists for this track
		uint64_t track = (pos >> 15) & 0x7F;
		uint64_t trackAddr;
		if (node->entries[track] == 0)
		{
			// we need to load the track
			uint64_t physStartFrame = phmAllocFrameEx(8, 0);
			if (physStartFrame == 0)
			{
				mutexUnlock(&sd->cacheLock);
				
				if (sizeWritten == 0)
				{
					ERRNO = ENOMEM;
					return -1;
				};
				
				break;
			};
			
			Semaphore semCmd;
			semInit2(&semCmd, 0);
			
			int status = 0;
			
			SDCommand *cmd = NEW(SDCommand);
			cmd->type = SD_CMD_READ_TRACK;
			cmd->block = (void*) (physStartFrame << 12);
			cmd->pos = pos & ~0x7FFFUL;
			cmd->cmdlock = &semCmd;
			cmd->status = &status;
			cmd->flags = 0;
			sdPush(sd, cmd);
			
			semWait(&semCmd);
			
			if (status != 0)
			{
				mutexUnlock(&sd->cacheLock);
				
				if (sizeWritten == 0)
				{
					ERRNO = EIO;
					return -1;
				};
				
				break;
			};
			
			void *vptr = mapPhysMemory(physStartFrame << 12, 0x8000);
			node->entries[track] = ((uint64_t) vptr & 0xFFFFFFFFFFFF) | (1UL << 56) | SD_BLOCK_DIRTY;
			trackAddr = (uint64_t) vptr;
		}
		else
		{
			trackAddr = (node->entries[track] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
			node->entries[track] |= SD_BLOCK_DIRTY;
		};
		
		memcpy((void*)(trackAddr + offsetIntoPage), scan, toWrite);
		scan += toWrite;
		sizeWritten += toWrite;
		pos += toWrite;
		size -= toWrite;
		
		mutexUnlock(&sd->cacheLock);
	};

	return sizeWritten;
};

static ssize_t sdfile_pread(Inode *inode, File *fp, void *buf, size_t size, off_t offset)
{
	SDHandle *handle = (SDHandle*) fp->filedata;
	uint64_t actualStart = handle->offset + (uint64_t) offset;
	if (handle->size != 0)
	{
		if (offset >= handle->size)
		{
			return 0;
		};
		
		if ((offset+size) > handle->size)
		{
			size = handle->size - offset;
		};
	};
	return sdRead(handle->sd, actualStart, buf, size);
};

static ssize_t sdfile_pwrite(Inode *inode, File *fp, const void *buf, size_t size, off_t offset)
{
	SDHandle *handle = (SDHandle*) fp->filedata;
	uint64_t actualStart = (uint64_t) handle->offset + (uint64_t) offset;
	if (handle->size != 0)
	{
		if (offset >= handle->size)
		{
			return 0;
		};
		
		if ((offset+size) > handle->size)
		{
			size = handle->size - offset;
		};
	};
	return sdWrite(handle->sd, actualStart, buf, size);
};

static void* sdfile_open(Inode *inode, int oflags)
{
	SDHandle *handle = NEW(SDHandle);
	if (handle == NULL)
	{
		ERRNO = ENOMEM;
		return NULL;
	};
	
	SDDeviceFile *fdev = (SDDeviceFile*) inode->fsdata;
	mutexLock(&fdev->sd->lock);
	if (fdev->partIndex == -1)
	{
		if (fdev->sd->openParts != 0)
		{
			mutexUnlock(&fdev->sd->lock);
			ERRNO = EBUSY;
			return NULL;
		}
		else
		{
			fdev->sd->openParts = SD_MASTER_OPEN;
		};
	}
	else
	{
		uint64_t mask = 1UL << (uint64_t)fdev->partIndex;
		if (fdev->sd->openParts & mask)
		{
			mutexUnlock(&fdev->sd->lock);
			ERRNO = EBUSY;
			return NULL;
		}
		else
		{
			fdev->sd->openParts |= mask;
		};
	};
	mutexUnlock(&fdev->sd->lock);
	
	sdUpref(fdev->sd);
	
	handle->sd = fdev->sd;
	handle->offset = fdev->offset;
	handle->size = fdev->size;
	handle->partIndex = fdev->partIndex;
	
	return handle;
};

static size_t sdfile_getsize(Inode *inode)
{
	SDDeviceFile *fdev = (SDDeviceFile*) inode->fsdata;
	mutexLock(&fdev->sd->lock);
	
	size_t size = fdev->sd->totalSize;
	if (fdev->size != 0)
	{
		size = fdev->size;
		mutexUnlock(&fdev->sd->lock);
	}
	else
	{
		if (size == 0)
		{
			Semaphore lock;
			semInit2(&lock, 0);

			SDCommand *cmd = (SDCommand*) kmalloc(sizeof(SDCommand));
			cmd->type = SD_CMD_GET_SIZE;
			cmd->block = &size;
			cmd->cmdlock = &lock;
			cmd->flags = 0;

			sdPush(fdev->sd, cmd);
			mutexUnlock(&fdev->sd->lock);
			semWait(&lock);		// cmd freed by the driver
		}
		else
		{
			mutexUnlock(&fdev->sd->lock);
		};
	};
	
	return size;
};

static Inode* sdCreateInode(SDDeviceFile *fdev)
{
	Inode *inode = vfsCreateInode(NULL, VFS_MODE_BLKDEV | 0600);
	inode->fsdata = fdev;
	inode->open = sdfile_open;
	inode->close = sdfile_close;
	inode->pread = sdfile_pread;
	inode->pwrite = sdfile_pwrite;
	inode->flush = sdfile_flush;
	inode->ioctl = sdfile_ioctl;
	inode->getsize = sdfile_getsize;
	return inode;
};

static void reloadPartTable(StorageDevice *sd)
{
	// delete current device files
	mutexLock(&sd->lock);
	size_t numRefs = sd->numSubs;
	
	int i;
	for (i=0; i<sd->numSubs; i++)
	{
		//DeleteDevice(sd->devSubs[i]);
		char subname[256];
		strformat(subname, 256, "sd%c%d", sd->letter, (int) i);
		devfsRemove(subname);
	};
	
	//kfree(sd->devSubs);
	//sd->devSubs = NULL;
	sd->numSubs = 0;

	while (numRefs--)
	{
		sdDownref(sd);
	};

	mutexUnlock(&sd->lock);

	// load the new partition table
	MBRPartition mbrParts[4];
	sdRead(sd, 0x1BE, mbrParts, 64);
	
	// make sure the boot signature is there (indicating an MBR)
	uint16_t sig;
	sdRead(sd, 0x1FE, &sig, 2);
	if (sig != 0xAA55)
	{
		// not an MBR
		return;
	};
	
	// we preallocate an array of 4 partition descriptions, even if they won't all be used
	mutexLock(&sd->lock);
	//sd->devSubs = (Device*) kmalloc(sizeof(Device)*4);
	sd->numSubs = 0;
	mutexUnlock(&sd->lock);
	
	int nextSubIndex = 0;
	for (i=0; i<4; i++)
	{
		if (mbrParts[i].systemID != 0)
		{
			SDDeviceFile *fdev = NEW(SDDeviceFile);
			fdev->sd = sd;
			fdev->offset = (uint64_t) mbrParts[i].lbaStart * 512;
			fdev->size = (uint64_t) mbrParts[i].numSectors * 512;
			fdev->partIndex = i;
			
			sdUpref(sd);

			char devName[16];
			strformat(devName, 16, "sd%c%d", sd->letter, nextSubIndex);
	
			mutexLock(&sd->lock);
			
			#if 0
			sd->devSubs[nextSubIndex] = AddDevice(devName, fdev, sdfile_open, 0600);
			if (sd->devMaster == NULL)
			{
				kfree(fdev);
			}
			else
			{
				nextSubIndex++;
			};
			#endif
			
			Inode *inode = /*vfsCreateInode(NULL, VFS_MODE_BLKDEV | 0600)*/ sdCreateInode(fdev);
			
			if (devfsAdd(devName, inode) != 0)
			{
				kfree(fdev);
			}
			else
			{
				nextSubIndex++;
			};
			
			mutexUnlock(&sd->lock);
		};
	}; 
	
	sd->numSubs = (size_t) nextSubIndex;
};

static void sdFlushThread(void *context)
{
	// already upreffed for us
	StorageDevice *sd = (StorageDevice*) context;
	
	while (1)
	{
		uint64_t nanotimeout = NT_SECS(120);
		int status = semWaitGen(&sd->semFlush, 1, 0, nanotimeout);
		
		if (status == 1)
		{
			break;
		}
		else if (status == -ETIMEDOUT)
		{
			mutexLock(&sd->cacheLock);
			sdFlush(sd);
			mutexUnlock(&sd->cacheLock);
		};
	};
};

StorageDevice* sdCreate(SDParams *params)
{
	char letter = sdAllocLetter();
	if (letter == 0)
	{
		return NULL;
	};
	
	StorageDevice *sd = NEW(StorageDevice);
	if (sd == NULL)
	{
		sdFreeLetter(letter);
		return NULL;
	};
	
	mutexInit(&sd->lock);
	sd->refcount = 1;
	sd->flags = params->flags;
	sd->blockSize = params->blockSize;
	sd->totalSize = params->totalSize;
	sd->letter = letter;
	sd->numSubs = 0;
	//sd->devSubs = NULL;
	sd->cmdq = NULL;
	semInit2(&sd->semCommands, 0);
	sd->openParts = 0;
	semInit2(&sd->semFlush, 0);
	
	sdUpref(sd);				// for the flush thread
	KernelThreadParams pars;
	memset(&pars, 0, sizeof(KernelThreadParams));
	pars.stackSize = DEFAULT_STACK_SIZE;
	pars.name = "SDI Flush Thread";
	sd->threadFlush = CreateKernelThread(sdFlushThread, &pars, sd);
	
	mutexInit(&sd->cacheLock);
	memset(&sd->cacheTop, 0, sizeof(BlockTreeNode));
	
	// master device file
	SDDeviceFile *fdev = NEW(SDDeviceFile);
	if (fdev == NULL)
	{
		sdFreeLetter(letter);
		kfree(sd);
		return NULL;
	};
	
	fdev->sd = sd;
	fdev->offset = 0;
	fdev->size = sd->totalSize;
	fdev->partIndex = -1;
	
	char masterName[16];
	strformat(masterName, 16, "sd%c", letter);
	
	mutexLock(&sd->lock);
	mutexLock(&sd->cacheLock);
	
	//sd->devMaster = AddDevice(masterName, fdev, sdfile_open, 0600);
	//if (sd->devMaster == NULL)
	//{
	//	kfree(sd);
	//	kfree(fdev);
	//	sdFreeLetter(letter);
	//	return NULL;
	//};
	
	Inode *inode = /*vfsCreateInode(NULL, VFS_MODE_BLKDEV | 0600) */ sdCreateInode(fdev);
	
	if (devfsAdd(masterName, inode) != 0)
	{
		kfree(sd);
		kfree(fdev);
		sdFreeLetter(letter);
		vfsDownrefInode(inode);
		return NULL;
	};

	mutexUnlock(&sd->cacheLock);
	mutexUnlock(&sd->lock);
	
	mutexLock(&mtxList);
	sdList[letter-'a'] = sd;
	mutexUnlock(&mtxList);
	
	return sd;
};

void sdHangup(StorageDevice *sd)
{	
	mutexLock(&sd->lock);
	
	char masterName[256];
	strformat(masterName, 256, "sd%c", sd->letter);
	
	//DeleteDevice(sd->devMaster);
	devfsRemove(masterName);
	size_t numRefs = 1 + sd->numSubs;
	
	size_t i;
	for (i=0; i<sd->numSubs; i++)
	{
		//DeleteDevice(sd->devSubs[i]);
		
		char subname[256];
		strformat(subname, 256, "sd%c%d", sd->letter, (int) i);
		
		devfsRemove(subname);
	};
	
	//kfree(sd->devSubs);
	//sd->devSubs = NULL;
	sd->numSubs = 0;
	//sd->devMaster = NULL;
	sdFreeLetter(sd->letter);
	sd->letter = 0;

	sd->flags |= SD_HANGUP;
	semSignal(&sd->semFlush);
	ReleaseKernelThread(sd->threadFlush);
	mutexUnlock(&sd->lock);
	
	mutexLock(&mtxList);
	sdList[sd->letter-'a'] = NULL;
	mutexUnlock(&mtxList);
	
	while (numRefs--)
	{
		sdDownref(sd);
	};
};

SDCommand* sdPop(StorageDevice *sd)
{
	semWait(&sd->semCommands);
	
	mutexLock(&sd->lock);
	SDCommand *cmd = sd->cmdq;
	sd->cmdq = cmd->next;
	cmd->next = NULL;
	mutexUnlock(&sd->lock);
	
	return cmd;
};

void sdPostComplete(SDCommand *cmd)
{
	if (cmd->flags & SD_CMD_NOFREE)
	{
		if (cmd->cmdlock != NULL) semSignal(cmd->cmdlock);
	}
	else
	{
		if (cmd->cmdlock != NULL) semSignal(cmd->cmdlock);
		kfree(cmd);
	};
};

void sdSignal(StorageDevice *dev)
{
	SDCommand *cmd = (SDCommand*) kmalloc(sizeof(SDCommand));
	cmd->type = SD_CMD_SIGNAL;
	cmd->cmdlock = NULL;
	cmd->flags = 0;
	
	sdPush(dev, cmd);
};

void sdSync()
{
	mutexLock(&mtxList);
	
	size_t i;
	for (i=0; i<26; i++)
	{
		if (sdList[i] != NULL)
		{
			StorageDevice *sd = sdList[i];
			mutexLock(&sd->cacheLock);
			sdFlush(sd);
			mutexUnlock(&sd->cacheLock);
		};
	};
	
	mutexUnlock(&mtxList);
};

static int sdTryFree(StorageDevice *sd, BlockTreeNode *node, int level, uint64_t addr)
{
	while (1)
	{
		uint64_t i;
	
		uint64_t lowestUsage;
		uint64_t lowestIndex;
		int foundAny = 0;
	
		for (i=0; i<128; i++)
		{
			if (node->entries[i] != 0)
			{
				if (!foundAny)
				{
					lowestUsage = node->entries[i] >> 56;
					lowestIndex = i;
					foundAny = 1;
				}
				else if ((node->entries[i] >> 56) < lowestUsage)
				{
					lowestUsage = node->entries[i] >> 56;
					lowestIndex = i;
				};
			};
		};
	
		// we cached nothing at this level, report failure
		if (!foundAny)
		{
			return -1;
		};
	
		if (level == 6)
		{
			if (node->entries[lowestIndex] & SD_BLOCK_DIRTY)
			{
				uint64_t canaddr = (node->entries[lowestIndex] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
				uint64_t *pte = (uint64_t*) ((canaddr >> 9) | 0xffffff8000000000L);
				uint64_t phys = (*pte) & 0x0000fffffffff000L;
			
				Semaphore semCmd;
				semInit2(&semCmd, 0);
			
				SDCommand cmd;
				cmd.type = SD_CMD_WRITE_TRACK;
				cmd.block = (void*) phys;
				cmd.pos = ((addr << 7) | lowestIndex) << 15;
				cmd.cmdlock = &semCmd;
				cmd.status = NULL;
				cmd.flags = SD_CMD_NOFREE;
				sdPush(sd, &cmd);
			
				semWait(&semCmd);
			};
		
			uint64_t canaddr = (node->entries[lowestIndex] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
			uint64_t *pte = (uint64_t*) ((canaddr >> 9) | 0xffffff8000000000L);
			uint64_t phys = (*pte) & 0x0000fffffffff000L;
		
			node->entries[lowestIndex] = 0;
			phmFreeFrameEx(phys >> 12, 8);
			unmapPhysMemory((void*)canaddr, 0x8000);
			
			return 0;
		}
		else
		{
			uint64_t canaddr = (node->entries[lowestIndex] & 0xFFFFFFFFFFFF) | 0xFFFF800000000000;
			int status = sdTryFree(sd, (BlockTreeNode*) canaddr, level+1, (addr << 7) | lowestIndex);
			
			if (status == 0)
			{
				return 0;
			}
			else
			{
				kfree((void*)canaddr);
				node->entries[lowestIndex] = 0;
				// and try again
			};
		};
	};
};

int sdFreeMemory()
{
	mutexLock(&mtxList);
	int status = -1;
	
	int i;
	for (i=0; i<26; i++)
	{
		StorageDevice *sd = sdList[i];
		if (sd != NULL)
		{
			mutexLock(&sd->cacheLock);
			status = sdTryFree(sd, &sd->cacheTop, 0, 0);
			mutexUnlock(&sd->cacheLock);
		
			if (status == 0) break;
		};
	};
	
	mutexUnlock(&mtxList);
	return status;
};