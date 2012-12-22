//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//					        FAT16/32 File IO Library
//								    V2.6
// 	  							 Rob Riglar
//						    Copyright 2003 - 2010
//
//   					  Email: rob@robriglar.com
//
//								License: GPL
//   If you would like a version with a more permissive license for use in
//   closed source commercial applications please contact me for details.
//-----------------------------------------------------------------------------
//
// This file is part of FAT File IO Library.
//
// FAT File IO Library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// FAT File IO Library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with FAT File IO Library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "fat_defs.h"
#include "fat_access.h"
#include "fat_table.h"
#include "fat_write.h"
#include "fat_misc.h"
#include "fat_string.h"
#include "fat_filelib.h"
#include "fat_cache.h"

//-----------------------------------------------------------------------------
// Locals
//-----------------------------------------------------------------------------
static FL_FILE			_files[FATFS_MAX_OPEN_FILES];
static int				_filelib_init = 0;
static int				_filelib_valid = 0;
static struct fatfs		_fs;
static FL_FILE*			_open_file_list = NULL;
static FL_FILE*			_free_file_list = NULL;

//-----------------------------------------------------------------------------
// Macros
//-----------------------------------------------------------------------------

// Macro for checking if file lib is initialised
#define CHECK_FL_INIT()		{ if (_filelib_init==0) fl_init(); }

#define FL_LOCK(a)			do { if ((a)->fl_lock) (a)->fl_lock(); } while (0)
#define FL_UNLOCK(a)		do { if ((a)->fl_unlock) (a)->fl_unlock(); } while (0)

//-----------------------------------------------------------------------------
// Local Functions
//-----------------------------------------------------------------------------
static void				_fl_init();

//-----------------------------------------------------------------------------
// _allocate_file: Find a slot in the open files buffer for a new file
//-----------------------------------------------------------------------------
static FL_FILE* _allocate_file(void)
{
	// Allocate free file
	FL_FILE* file = _free_file_list;

	if (file)
	{
		_free_file_list = file->next;

		// Add to open list
		file->next = _open_file_list;
		_open_file_list = file;
	}

	return file;
}
//-----------------------------------------------------------------------------
// _check_file_open: Returns true if the file is already open
//-----------------------------------------------------------------------------
static int _check_file_open(FL_FILE* file)
{
	FL_FILE* openFile = _open_file_list;
	
	// Compare open files
	while (openFile)
	{
		// If not the current file 
		if (openFile != file)
		{
			// Compare path and name
			if ( (fatfs_compare_names(openFile->path,file->path)) && (fatfs_compare_names(openFile->filename,file->filename)) )
				return 1;
		}

		openFile = openFile->next;
	}

	return 0;
}
//-----------------------------------------------------------------------------
// _free_file: Free open file handle
//-----------------------------------------------------------------------------
static void _free_file(FL_FILE* file)
{
	FL_FILE* openFile = _open_file_list;
	FL_FILE* lastFile = NULL;
	
	// Remove from open list
	while (openFile)
	{
		// If the current file 
		if (openFile == file)
		{
			if (lastFile)
				lastFile->next = openFile->next;
			else
				_open_file_list = openFile->next;

			break;
		}

		lastFile = openFile;
		openFile = openFile->next;
	}

	// Add to free list
	file->next = _free_file_list;
	_free_file_list = file;
}

//-----------------------------------------------------------------------------
//								Low Level
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// _open_directory: Cycle through path string to find the start cluster
// address of the highest subdir.
//-----------------------------------------------------------------------------
static int _open_directory(char *path, unsigned long *pathCluster)
{
	int levels;
	int sublevel;
	char currentfolder[FATFS_MAX_LONG_FILENAME];
	struct fat_dir_entry sfEntry;
	unsigned long startcluster;

	// Set starting cluster to root cluster
	startcluster = fatfs_get_root_cluster(&_fs);

	// Find number of levels
	levels = fatfs_total_path_levels(path);

	// Cycle through each level and get the start sector
	for (sublevel=0;sublevel<(levels+1);sublevel++) 
	{
		if (fatfs_get_substring(path, sublevel, currentfolder, sizeof(currentfolder)) == -1)
			return 0;

		// Find clusteraddress for folder (currentfolder) 
		if (fatfs_get_file_entry(&_fs, startcluster, currentfolder,&sfEntry))
		{
			// Check entry is folder
			if (fatfs_entry_is_dir(&sfEntry))
				startcluster = ((FAT_HTONS((unsigned long)sfEntry.FstClusHI))<<16) + FAT_HTONS(sfEntry.FstClusLO);
			else
				return 0;
		}
		else
			return 0;
	}

	*pathCluster = startcluster;
	return 1;
}
//-----------------------------------------------------------------------------
// _create_directory: Cycle through path string and create the end directory
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
static int _create_directory(char *path)
{
	FL_FILE* file; 
	struct fat_dir_entry sfEntry;
	char shortFilename[FAT_SFN_SIZE_FULL];
	int tailNum = 0;
	int i;

	// Allocate a new file handle
	file = _allocate_file();
	if (!file)
		return 0;

	// Clear filename
	memset(file->path, '\0', sizeof(file->path));
	memset(file->filename, '\0', sizeof(file->filename));

	// Split full path into filename and directory path
	if (fatfs_split_path((char*)path, file->path, sizeof(file->path), file->filename, sizeof(file->filename)) == -1)
	{
		_free_file(file);
		return 0;
	}

	// Check if file already open
	if (_check_file_open(file))
	{
		_free_file(file);
		return 0;
	}

	// If file is in the root dir
	if (file->path[0] == 0)
		file->parentcluster = fatfs_get_root_cluster(&_fs);
	else
	{
		// Find parent directory start cluster
		if (!_open_directory(file->path, &file->parentcluster))
		{
			_free_file(file);
			return 0;
		}
	}

	// Check if same filename exists in directory
	if (fatfs_get_file_entry(&_fs, file->parentcluster, file->filename,&sfEntry) == 1)
	{
		_free_file(file);
		return 0;
	}

	file->startcluster = 0;

	// Create the file space for the folder (at least one clusters worth!)
	if (!fatfs_allocate_free_space(&_fs, 1, &file->startcluster, 1))
	{
		_free_file(file);
		return 0;
	}

	// Erase new directory cluster
	memset(file->file_data.sector, 0x00, FAT_SECTOR_SIZE);
	for (i=0;i<_fs.sectors_per_cluster;i++)
	{
		if (!fatfs_write_sector(&_fs, file->startcluster, i, file->file_data.sector))
		{
			_free_file(file);
			return 0;
		}
	}

#if FATFS_INC_LFN_SUPPORT

	// Generate a short filename & tail
	tailNum = 0;
	do 
	{
		// Create a standard short filename (without tail)
		fatfs_lfn_create_sfn(shortFilename, file->filename);

        // If second hit or more, generate a ~n tail		
		if (tailNum != 0)
			fatfs_lfn_generate_tail((char*)file->shortfilename, shortFilename, tailNum);
		// Try with no tail if first entry
		else
			memcpy(file->shortfilename, shortFilename, FAT_SFN_SIZE_FULL);

		// Check if entry exists already or not
		if (fatfs_sfn_exists(&_fs, file->parentcluster, (char*)file->shortfilename) == 0)
			break;

		tailNum++;
	}
	while (tailNum < 9999);

	// We reached the max number of duplicate short file names (unlikely!)
	if (tailNum == 9999)
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return 0;
	}
#else
	// Create a standard short filename (without tail)
	if (!fatfs_lfn_create_sfn(shortFilename, file->filename))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return 0;
	}

	// Copy to SFN space
	memcpy(file->shortfilename, shortFilename, FAT_SFN_SIZE_FULL);

	// Check if entry exists already
	if (fatfs_sfn_exists(&_fs, file->parentcluster, (char*)file->shortfilename))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return 0;
	}
#endif

	// Add file to disk
	if (!fatfs_add_file_entry(&_fs, file->parentcluster, (char*)file->filename, (char*)file->shortfilename, file->startcluster, 0, 1))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return 0;
	}

	// General
	file->filelength = 0;
	file->bytenum = 0;
	file->file_data.address = 0xFFFFFFFF;
	file->file_data.dirty = 0;
	file->filelength_changed = 0;
	
	// Quick lookup for next link in the chain
	file->last_fat_lookup.ClusterIdx = 0xFFFFFFFF;
	file->last_fat_lookup.CurrentCluster = 0xFFFFFFFF;
	
	fatfs_fat_purge(&_fs);

	_free_file(file);
	return 1;
}
#endif
//-----------------------------------------------------------------------------
// _open_file: Open a file for reading
//-----------------------------------------------------------------------------
static FL_FILE* _open_file(const char *path, int checkfile)
{
	FL_FILE* file; 
	struct fat_dir_entry sfEntry;

	// Allocate a new file handle
	file = _allocate_file();
	if (!file)
		return NULL;

	// Clear filename
	memset(file->path, '\0', sizeof(file->path));
	memset(file->filename, '\0', sizeof(file->filename));

	// Split full path into filename and directory path
	if (fatfs_split_path((char*)path, file->path, sizeof(file->path), file->filename, sizeof(file->filename)) == -1)
	{
		_free_file(file);
		return NULL;
	}

	// Check if file already open
	if (_check_file_open(file))
	{
		_free_file(file);
		return NULL;
	}

	// If file is in the root dir
	if (file->path[0]==0)
		file->parentcluster = fatfs_get_root_cluster(&_fs);
	else
	{
		// Find parent directory start cluster
		if (!_open_directory(file->path, &file->parentcluster))
		{
			_free_file(file);
			return NULL;
		}
	}

	// Using dir cluster address search for filename
	if (fatfs_get_file_entry(&_fs, file->parentcluster, file->filename,&sfEntry))
	{
		// Make sure entry is file not dir!
		if(!checkfile || fatfs_entry_is_file(&sfEntry))
		{
			// Initialise file details
			memcpy(file->shortfilename, sfEntry.Name, FAT_SFN_SIZE_FULL);
			file->filelength = FAT_HTONL(sfEntry.FileSize);
			file->bytenum = 0;
			file->startcluster = ((FAT_HTONS((unsigned long)sfEntry.FstClusHI))<<16) + FAT_HTONS(sfEntry.FstClusLO);
			file->file_data.address = 0xFFFFFFFF;
			file->file_data.dirty = 0;
			file->filelength_changed = 0;

			// Quick lookup for next link in the chain
			file->last_fat_lookup.ClusterIdx = 0xFFFFFFFF;
			file->last_fat_lookup.CurrentCluster = 0xFFFFFFFF;

			fatfs_cache_init(&_fs, file);

			fatfs_fat_purge(&_fs);

			return file;
		}
	}

	_free_file(file);
	return NULL;
}
//-----------------------------------------------------------------------------
// _create_file: Create a new file
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
static FL_FILE* _create_file(const char *filename)
{
	FL_FILE* file; 
	struct fat_dir_entry sfEntry;
	char shortFilename[FAT_SFN_SIZE_FULL];
	int tailNum = 0;

	// No write access?
	if (!_fs.disk_io.write_sector)
		return NULL;

	// Allocate a new file handle
	file = _allocate_file();
	if (!file)
		return NULL;

	// Clear filename
	memset(file->path, '\0', sizeof(file->path));
	memset(file->filename, '\0', sizeof(file->filename));

	// Split full path into filename and directory path
	if (fatfs_split_path((char*)filename, file->path, sizeof(file->path), file->filename, sizeof(file->filename)) == -1)
	{
		_free_file(file);
		return NULL;
	}

	// Check if file already open
	if (_check_file_open(file))
	{
		_free_file(file);
		return NULL;
	}

	// If file is in the root dir
	if (file->path[0] == 0)
		file->parentcluster = fatfs_get_root_cluster(&_fs);
	else
	{
		// Find parent directory start cluster
		if (!_open_directory(file->path, &file->parentcluster))
		{
			_free_file(file);
			return NULL;
		}
	}

	// Check if same filename exists in directory
	if (fatfs_get_file_entry(&_fs, file->parentcluster, file->filename,&sfEntry) == 1)
	{
		_free_file(file);
		return NULL;
	}

	file->startcluster = 0;

	// Create the file space for the file (at least one clusters worth!)
	if (!fatfs_allocate_free_space(&_fs, 1, &file->startcluster, 1))
	{
		_free_file(file);
		return NULL;
	}

#if FATFS_INC_LFN_SUPPORT
	// Generate a short filename & tail
	tailNum = 0;
	do 
	{
		// Create a standard short filename (without tail)
		fatfs_lfn_create_sfn(shortFilename, file->filename);

        // If second hit or more, generate a ~n tail		
		if (tailNum != 0)
			fatfs_lfn_generate_tail((char*)file->shortfilename, shortFilename, tailNum);
		// Try with no tail if first entry
		else
			memcpy(file->shortfilename, shortFilename, FAT_SFN_SIZE_FULL);

		// Check if entry exists already or not
		if (fatfs_sfn_exists(&_fs, file->parentcluster, (char*)file->shortfilename) == 0)
			break;

		tailNum++;
	}
	while (tailNum < 9999);

	// We reached the max number of duplicate short file names (unlikely!)
	if (tailNum == 9999)
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return NULL;
	}
#else
	// Create a standard short filename (without tail)
	if (!fatfs_lfn_create_sfn(shortFilename, file->filename))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return NULL;
	}

	// Copy to SFN space
	memcpy(file->shortfilename, shortFilename, FAT_SFN_SIZE_FULL);

	// Check if entry exists already
	if (fatfs_sfn_exists(&_fs, file->parentcluster, (char*)file->shortfilename))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return NULL;
	}
#endif

	// Add file to disk
	if (!fatfs_add_file_entry(&_fs, file->parentcluster, (char*)file->filename, (char*)file->shortfilename, file->startcluster, 0, 0))
	{
		// Delete allocated space
		fatfs_free_cluster_chain(&_fs, file->startcluster);

		_free_file(file);
		return NULL;
	}

	// General
	file->filelength = 0;
	file->bytenum = 0;
	file->file_data.address = 0xFFFFFFFF;
	file->file_data.dirty = 0;
	file->filelength_changed = 0;

	// Quick lookup for next link in the chain
	file->last_fat_lookup.ClusterIdx = 0xFFFFFFFF;
	file->last_fat_lookup.CurrentCluster = 0xFFFFFFFF;

	fatfs_cache_init(&_fs, file);
	
	fatfs_fat_purge(&_fs);

	return file;
}
#endif
//-----------------------------------------------------------------------------
// _read_sector: Read a sector from disk to file
//-----------------------------------------------------------------------------
static int _read_sector(FL_FILE* file, UINT32 offset, unsigned char *buffer)
{
	UINT32 Sector = 0;
	UINT32 ClusterIdx = 0;
	UINT32 Cluster = 0;
	UINT32 i;
	UINT32 lba;

	// Find cluster index within file & sector with cluster
	ClusterIdx = offset / _fs.sectors_per_cluster;	  
	Sector = offset - (ClusterIdx * _fs.sectors_per_cluster);

	// Quick lookup for next link in the chain
	if (ClusterIdx == file->last_fat_lookup.ClusterIdx)
		Cluster = file->last_fat_lookup.CurrentCluster;
	// Else walk the chain
	else
	{
		// Starting from last recorded cluster?
		if (ClusterIdx && ClusterIdx == file->last_fat_lookup.ClusterIdx + 1)
		{
			i = file->last_fat_lookup.ClusterIdx;
			Cluster = file->last_fat_lookup.CurrentCluster;
		}
		// Start searching from the beginning..
		else
		{
			// Set start of cluster chain to initial value
			i = 0;
			Cluster = file->startcluster;					
		}

		// Follow chain to find cluster to read
		for ( ;i<ClusterIdx; i++)
		{
			UINT32 nextCluster;
			
			// Does the entry exist in the cache?
			if (!fatfs_cache_get_next_cluster(&_fs, file, i, &nextCluster))			
			{
				// Scan file linked list to find next entry
				nextCluster = fatfs_find_next_cluster(&_fs, Cluster);

				// Push entry into cache
				fatfs_cache_set_next_cluster(&_fs, file, i, nextCluster);
			}			

			Cluster = nextCluster;
		}

		// Record current cluster lookup details (if valid)
		if (Cluster != FAT32_LAST_CLUSTER)
		{
			file->last_fat_lookup.CurrentCluster = Cluster;
			file->last_fat_lookup.ClusterIdx = ClusterIdx;
		}
	}

	// If end of cluster chain then return false
	if (Cluster == FAT32_LAST_CLUSTER) 
		return 0;

	// Calculate sector address
	lba = fatfs_lba_of_cluster(&_fs, Cluster) + Sector;

	// Read sector of file
	return fatfs_sector_read(&_fs, lba, buffer);
}

//-----------------------------------------------------------------------------
//								External API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// fl_init: Initialise library
//-----------------------------------------------------------------------------
void fl_init(void)
{	
	int i;

	// Add all file objects to free list
	for (i=0;i<FATFS_MAX_OPEN_FILES;i++)
	{
		_files[i].next = _free_file_list;
		_free_file_list = &_files[i];
	}

	_filelib_init = 1;
}
//-----------------------------------------------------------------------------
// fl_attach_locks: 
//-----------------------------------------------------------------------------
void fl_attach_locks(struct fatfs *fs, void (*lock)(void), void (*unlock)(void))
{
	fs->fl_lock = lock;
	fs->fl_unlock = unlock;
}
//-----------------------------------------------------------------------------
// fl_attach_media: 
//-----------------------------------------------------------------------------
int fl_attach_media(fn_diskio_read rd, fn_diskio_write wr)
{
	int res;

	// If first call to library, initialise
	CHECK_FL_INIT();

	_fs.disk_io.read_sector = rd;
	_fs.disk_io.write_sector = wr;

	// Initialise FAT parameters
	if ((res = fatfs_init(&_fs)) != FAT_INIT_OK)
	{
		FAT_PRINTF(("FAT_FS: Error could not load FAT details (%d)!\r\n", res));
		return res;
	}

	_filelib_valid = 1;
	return FAT_INIT_OK;
}
//-----------------------------------------------------------------------------
// fl_shutdown: Call before shutting down system
//-----------------------------------------------------------------------------
void fl_shutdown(void)
{
	// If first call to library, initialise
	CHECK_FL_INIT();

	FL_LOCK(&_fs);
	fatfs_fat_purge(&_fs);
	FL_UNLOCK(&_fs);
}
//-----------------------------------------------------------------------------
// fl_ifopen: Internal helper function for fl_fopen and fl_remove
//-----------------------------------------------------------------------------
void* fl_ifopen(const char *path, const char *mode, int checkfile)
{
	int i;
	FL_FILE* file; 
	unsigned char flags = 0;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (!_filelib_valid)
		return NULL;

	if (!path || !mode)
		return NULL;

	// Supported Modes:
	// "r" Open a file for reading. 
	//		The file must exist. 
	// "w" Create an empty file for writing. 
	//		If a file with the same name already exists its content is erased and the file is treated as a new empty file.  
	// "a" Append to a file. 
	//		Writing operations append data at the end of the file. 
	//		The file is created if it does not exist. 
	// "r+" Open a file for update both reading and writing. 
	//		The file must exist. 
	// "w+" Create an empty file for both reading and writing. 
	//		If a file with the same name already exists its content is erased and the file is treated as a new empty file. 
	// "a+" Open a file for reading and appending. 
	//		All writing operations are performed at the end of the file, protecting the previous content to be overwritten. 
	//		You can reposition (fseek, rewind) the internal pointer to anywhere in the file for reading, but writing operations 
	//		will move it back to the end of file. 
	//		The file is created if it does not exist. 

	for (i=0;i<(int)strlen(mode);i++)
	{
		switch (tolower(mode[i]))
		{
		case 'r':
			flags |= FILE_READ;
			break;
		case 'w':
			flags |= FILE_WRITE;
			flags |= FILE_ERASE;
			flags |= FILE_CREATE;
			break;
		case 'a':
			flags |= FILE_WRITE;
			flags |= FILE_APPEND;
			flags |= FILE_CREATE;
			break;
		case '+':
			if (flags & FILE_READ)
				flags |= FILE_WRITE;
			else if (flags & FILE_WRITE)
			{
				flags |= FILE_READ;
				flags |= FILE_ERASE;
				flags |= FILE_CREATE;
			}
			else if (flags & FILE_APPEND)
			{
				flags |= FILE_READ;
				flags |= FILE_WRITE;
				flags |= FILE_APPEND;
				flags |= FILE_CREATE;
			}
			break;
		case 'b':
			flags |= FILE_BINARY;
			break;
		}
	}

	file = NULL;

#ifndef FATFS_INC_WRITE_SUPPORT
	// No write support!
	flags &= ~(FILE_CREATE | FILE_WRITE | FILE_APPEND);
#endif

	// No write access - remove write/modify flags
	if (!_fs.disk_io.write_sector)
		flags &= ~(FILE_CREATE | FILE_WRITE | FILE_APPEND);

	FL_LOCK(&_fs);

	// Read
	if (flags & FILE_READ)
		file = _open_file(path, checkfile);

	// Create New
#ifdef FATFS_INC_WRITE_SUPPORT
	if (!file && (flags & FILE_CREATE))
		file = _create_file(path);
#endif

	// Write Existing (and not open due to read or create)
	if (!(flags & FILE_READ))
		if ((flags & FILE_CREATE) && !file)
			if (flags & (FILE_WRITE | FILE_APPEND))
				file = _open_file(path, checkfile);

	if (file)
		file->flags = flags;

	FL_UNLOCK(&_fs);
	return file;	
}

//-----------------------------------------------------------------------------
// fl_fopen: Open or Create a file for reading or writing
//-----------------------------------------------------------------------------
void* fl_fopen(const char *path, const char *mode)
{
	return fl_ifopen(path, mode, 1);
}
//-----------------------------------------------------------------------------
// _write_sector: Write sector to disk
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
static int _write_sector(FL_FILE* file, UINT32 offset, unsigned char *buf)
{
	UINT32 SectorNumber = 0;
	UINT32 ClusterIdx = 0;
	UINT32 Cluster = 0;
	UINT32 LastCluster = FAT32_LAST_CLUSTER;
	UINT32 i;

	// Find values for Cluster index & sector within cluster
	ClusterIdx = offset / _fs.sectors_per_cluster;	  
	SectorNumber = offset - (ClusterIdx * _fs.sectors_per_cluster);

	// Quick lookup for next link in the chain
	if (ClusterIdx == file->last_fat_lookup.ClusterIdx)
		Cluster = file->last_fat_lookup.CurrentCluster;
	// Else walk the chain
	else
	{
		// Starting from last recorded cluster?
		if (ClusterIdx && ClusterIdx == file->last_fat_lookup.ClusterIdx + 1)
		{
			i = file->last_fat_lookup.ClusterIdx;
			Cluster = file->last_fat_lookup.CurrentCluster;
		}
		// Start searching from the beginning..
		else
		{
			// Set start of cluster chain to initial value
			i = 0;
			Cluster = file->startcluster;
		}

		// Follow chain to find cluster to read
		for ( ;i<ClusterIdx; i++)
		{
			UINT32 nextCluster;
			
			// Does the entry exist in the cache?
			if (!fatfs_cache_get_next_cluster(&_fs, file, i, &nextCluster))			
			{
				// Scan file linked list to find next entry
				nextCluster = fatfs_find_next_cluster(&_fs, Cluster);

				// Push entry into cache
				fatfs_cache_set_next_cluster(&_fs, file, i, nextCluster);
			}			

			LastCluster = Cluster;
			Cluster = nextCluster;

			// Dont keep following a dead end
			if (Cluster == FAT32_LAST_CLUSTER)
				break;
		}

		// If we have reached the end of the chain, allocate more!
		if (Cluster == FAT32_LAST_CLUSTER)
		{
			// Add another cluster to the last good cluster chain
			if (!fatfs_add_free_space(&_fs, &LastCluster))
				return 0;

			Cluster = LastCluster;
		}

		// Record current cluster lookup details
		file->last_fat_lookup.CurrentCluster = Cluster;
		file->last_fat_lookup.ClusterIdx = ClusterIdx;
	}

	return fatfs_write_sector(&_fs, Cluster, SectorNumber, buf);
}
#endif
//-----------------------------------------------------------------------------
// fl_fflush: Flush un-written data to the file
//-----------------------------------------------------------------------------
int fl_fflush(void *f)
{
#ifdef FATFS_INC_WRITE_SUPPORT
	FL_FILE *file = (FL_FILE *)f;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (file)
	{
		FL_LOCK(&_fs);

		// If some write data still in buffer
		if (file->file_data.dirty)
		{
			// Write back current sector before loading next
			if (_write_sector(file, file->file_data.address, file->file_data.sector))
				file->file_data.dirty = 0;
		}

		FL_UNLOCK(&_fs);
	}
#endif
	return 0;
}
//-----------------------------------------------------------------------------
// fl_fclose: Close an open file
//-----------------------------------------------------------------------------
void fl_fclose(void *f)
{
	FL_FILE *file = (FL_FILE *)f;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (file)
	{
		FL_LOCK(&_fs);

		// Flush un-written data to file
		fl_fflush(f);

		// File size changed?
		if (file->filelength_changed)
		{
#ifdef FATFS_INC_WRITE_SUPPORT
			// Update filesize in directory
			fatfs_update_file_length(&_fs, file->parentcluster, (char*)file->shortfilename, file->filelength);
#endif
			file->filelength_changed = 0;
		}

		file->bytenum = 0;
		file->filelength = 0;
		file->startcluster = 0;
		file->file_data.address = 0xFFFFFFFF;
		file->file_data.dirty = 0;
		file->filelength_changed = 0;

		// Free file handle
		_free_file(file);

		fatfs_fat_purge(&_fs);

		FL_UNLOCK(&_fs);
	}
}
//-----------------------------------------------------------------------------
// fl_fgetc: Get a character in the stream
//-----------------------------------------------------------------------------
int fl_fgetc(void *f)
{
	int res;
	unsigned char data = 0;
	
	res = fl_fread(&data, 1, 1, f);
	if (res == 1)
		return (int)data;
	else
		return res;
}
//-----------------------------------------------------------------------------
// fl_fread: Read a block of data from the file
//-----------------------------------------------------------------------------
int fl_fread(void * buffer, int size, int length, void *f )
{
	unsigned long sector;
	unsigned long offset;
	int copyCount;
	int count = size * length;
	int bytesRead = 0;	

	FL_FILE *file = (FL_FILE *)f;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (buffer==NULL || file==NULL)
		return -1;

	// No read permissions
	if (!(file->flags & FILE_READ))
		return -1;

	// Nothing to be done
	if (!count)
		return 0;

	// Check if read starts past end of file
	if (file->bytenum >= file->filelength)
		return -1;

	// Limit to file size
	if ( (file->bytenum + count) > file->filelength )
		count = file->filelength - file->bytenum;

	// Calculate start sector
	sector = file->bytenum / FAT_SECTOR_SIZE;

	// Offset to start copying data from first sector
	offset = file->bytenum % FAT_SECTOR_SIZE;

	while (bytesRead < count)
	{		
		// Do we need to re-read the sector?
		if (file->file_data.address != sector)
		{
			// Flush un-written data to file
			if (file->file_data.dirty)
				fl_fflush(file);

			// Get LBA of sector offset within file
			if (!_read_sector(file, sector, file->file_data.sector))
				// Read failed - out of range (probably)
				break;

			file->file_data.address = sector;
			file->file_data.dirty = 0;
		}

		// We have upto one sector to copy
		copyCount = FAT_SECTOR_SIZE - offset;

		// Only require some of this sector?
		if (copyCount > (count - bytesRead))
			copyCount = (count - bytesRead);

		// Copy to application buffer
		memcpy( (unsigned char*)((unsigned char*)buffer + bytesRead), (unsigned char*)(file->file_data.sector + offset), copyCount);
	
		// Increase total read count 
		bytesRead += copyCount;

		// Increment file pointer
		file->bytenum += copyCount;

		// Move onto next sector and reset copy offset
		sector++;
		offset = 0;
	}	

	return bytesRead;
}
//-----------------------------------------------------------------------------
// fl_fread_sector: Zero copy sector sized read function (Extension)
//-----------------------------------------------------------------------------
int fl_fread_sector(unsigned char *buffer, int size, int length, void *f )
{
	unsigned long sector;
	unsigned long offset;
	int copyCount;
	int count = size * length;
	int bytesRead = 0;	

	FL_FILE *file = (FL_FILE *)f;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (buffer==NULL || file==NULL || count != FAT_SECTOR_SIZE)
		return -1;

	// No read permissions
	if (!(file->flags & FILE_READ))
		return -1;

	// Check if read starts past end of file
	if (file->bytenum >= file->filelength)
		return -1;

	// Limit to file size
	if ( (file->bytenum + count) > file->filelength )
		count = file->filelength - file->bytenum;

	// Calculate start sector
	sector = file->bytenum / FAT_SECTOR_SIZE;

	// Offset to start copying data from first sector
	offset = file->bytenum % FAT_SECTOR_SIZE;

	// Needs to be sector aligned
	if (offset != 0)
		return -1;

	// Get LBA of sector offset within file
	if (_read_sector(file, sector, buffer))
	{
		// We have upto one sector to copy
		copyCount = FAT_SECTOR_SIZE;

		// Only require some of this sector?
		if (copyCount > (count - bytesRead))
			copyCount = (count - bytesRead);
	
		// Increase total read count 
		bytesRead = copyCount;

		// Increment file pointer
		file->bytenum += copyCount;
	}	

	return bytesRead;
}
//-----------------------------------------------------------------------------
// fl_fseek: Seek to a specific place in the file
//-----------------------------------------------------------------------------
int fl_fseek( void *f, long offset, int origin )
{
	FL_FILE *file = (FL_FILE *)f;
	int res = -1;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (!file)
		return -1;

	if (origin == SEEK_END && offset != 0)
		return -1;

	FL_LOCK(&_fs);

	// Invalidate file buffer
	file->file_data.address = 0xFFFFFFFF;
	file->file_data.dirty = 0;

	if (origin == SEEK_SET)
	{
		file->bytenum = (unsigned long)offset;

		if (file->bytenum > file->filelength)
			file->bytenum = file->filelength;

		res = 0;
	}
	else if (origin == SEEK_CUR)
	{
		// Positive shift
		if (offset >= 0)
		{
			file->bytenum += offset;

			if (file->bytenum > file->filelength)
				file->bytenum = file->filelength;
		}
		// Negative shift
		else
		{
			// Make shift positive
			offset = -offset;

			// Limit to negative shift to start of file
			if ((unsigned long)offset > file->bytenum)
				file->bytenum = 0;
			else
				file->bytenum-= offset;
		}

		res = 0;
	}
	else if (origin == SEEK_END)
	{
		file->bytenum = file->filelength;
		res = 0;
	}
	else
		res = -1;

	FL_UNLOCK(&_fs);

	return res;
}
//-----------------------------------------------------------------------------
// fl_fgetpos: Get the current file position
//-----------------------------------------------------------------------------
int fl_fgetpos(void *f , unsigned long * position)
{
	FL_FILE *file = (FL_FILE *)f;

	if (!file)
		return -1;

	FL_LOCK(&_fs);

	// Get position
	*position = file->bytenum;

	FL_UNLOCK(&_fs);

	return 0;
}
//-----------------------------------------------------------------------------
// fl_ftell: Get the current file position
//-----------------------------------------------------------------------------
long fl_ftell(void *f)
{
	unsigned long pos = 0;

	fl_fgetpos(f, &pos);

	return (long)pos;
}
//-----------------------------------------------------------------------------
// fl_feof: Is the file pointer at the end of the stream?
//-----------------------------------------------------------------------------
int fl_feof(void *f)
{
	FL_FILE *file = (FL_FILE *)f;
	int res;

	if (!file)
		return -1;

	FL_LOCK(&_fs);

	if (file->bytenum == file->filelength)
		res = EOF;
	else
		res = 0;

	FL_UNLOCK(&_fs);

	return res;
}
//-----------------------------------------------------------------------------
// fl_fputc: Write a character to the stream
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_fputc(int c, void *f)
{
	unsigned char data = (unsigned char)c;
	int res;

	res = fl_fwrite(&data, 1, 1, f);
	if (res == 1)
		return c;
	else
		return res;
}
#endif
//-----------------------------------------------------------------------------
// fl_fwrite: Write a block of data to the stream
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_fwrite(const void * data, int size, int count, void *f )
{
	FL_FILE *file = (FL_FILE *)f;
	unsigned long sector;
	unsigned long offset;	
	unsigned long length = (size*count);
	unsigned char *buffer = (unsigned char *)data;
	int dirtySector = 0;
	unsigned long bytesWritten = 0;
	unsigned long copyCount;

	// If first call to library, initialise
	CHECK_FL_INIT();

	if (!file)
		return -1;

	FL_LOCK(&_fs);

	// No write permissions
	if (!(file->flags & FILE_WRITE))
	{
		FL_UNLOCK(&_fs);
		return -1;
	}

	// Append writes to end of file
	if (file->flags & FILE_APPEND)
		file->bytenum = file->filelength;
	// Else write to current position

	// Calculate start sector
	sector = file->bytenum / FAT_SECTOR_SIZE;

	// Offset to start copying data from first sector
	offset = file->bytenum % FAT_SECTOR_SIZE;

	while (bytesWritten < length)
	{
		// We have upto one sector to copy
		copyCount = FAT_SECTOR_SIZE - offset;

		// Only require some of this sector?
		if (copyCount > (length - bytesWritten))
			copyCount = (length - bytesWritten);

		// Do we need to read a new sector?
		if (file->file_data.address != sector)
		{
			// Flush un-written data to file
			if (file->file_data.dirty)
				fl_fflush(file);

			// If we plan to overwrite the whole sector, we don't need to read it first!
			if (copyCount != FAT_SECTOR_SIZE)
			{
				// NOTE: This does not have succeed; if last sector of file
				// reached, no valid data will be read in, but write will 
				// allocate some more space for new data.

				// Get LBA of sector offset within file
				if (!_read_sector(file, sector, file->file_data.sector))
					memset(file->file_data.sector, 0x00, FAT_SECTOR_SIZE);	
			}

			file->file_data.address = sector;
			file->file_data.dirty = 0;
		}

		// Copy from application buffer into sector buffer
		memcpy((unsigned char*)(file->file_data.sector + offset), (unsigned char*)(buffer + bytesWritten), copyCount);

		// Mark buffer as dirty
		file->file_data.dirty = 1;
	
		// Increase total read count 
		bytesWritten += copyCount;

		// Increment file pointer
		file->bytenum += copyCount;

		// Move onto next sector and reset copy offset
		sector++;
		offset = 0;
	}

	// Write increased extent of the file?
	if (file->bytenum > file->filelength)
	{
		// Increase file size to new point
		file->filelength = file->bytenum;

		// We are changing the file length and this 
		// will need to be writen back at some point
		file->filelength_changed = 1;
	}

	FL_UNLOCK(&_fs);

	return (size*count);
}
#endif
//-----------------------------------------------------------------------------
// fl_fputs: Write a character string to the stream
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_fputs(const char * str, void *f)
{
	int len = (int)strlen(str);
	int res = fl_fwrite(str, 1, len, f);

	if (res == len)
		return len;
	else
		return res;
}
#endif
//-----------------------------------------------------------------------------
// fl_remove: Remove a file from the filesystem
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_remove( const char * filename )
{
	FL_FILE* file;
	int res = -1;

	FL_LOCK(&_fs);

	// Use read_file as this will check if the file is already open!
	file = fl_ifopen((char*)filename, "r", 0);
	if (file)
	{
		// Delete allocated space
		if (fatfs_free_cluster_chain(&_fs, file->startcluster))
		{
			// Remove directory entries
			if (fatfs_mark_file_deleted(&_fs, file->parentcluster, (char*)file->shortfilename))
			{
				// Close the file handle (this should not write anything to the file
				// as we have not changed the file since opening it!)
				fl_fclose(file);

				res = 0;
			}
		}
	}

	FL_UNLOCK(&_fs);

	return res;
}
#endif
//-----------------------------------------------------------------------------
// fl_fprintf: Write a variable argument character string to the stream
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_fprintf(void *file, const char * format, ...)
{
	char buf[16384];
	va_list args;
	int len;
	int res;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	len = (int)strlen(buf);
	res = fl_fwrite(buf, 1, len, file);

	if (res == len)
		return len;
	else
		return res;
}
#endif
//-----------------------------------------------------------------------------
// fl_createdirectory: Create a directory based on a path
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_WRITE_SUPPORT
int fl_createdirectory(const char *path)
{
	int res;

	// If first call to library, initialise
	CHECK_FL_INIT();

	FL_LOCK(&_fs);
	res =_create_directory((char*)path);
	FL_UNLOCK(&_fs);

	return res;
}
#endif
//-----------------------------------------------------------------------------
// fl_listdirectory: List a directory based on a path
//-----------------------------------------------------------------------------
#if FATFS_DIR_LIST_SUPPORT
void fl_listdirectory(const char *path)
{
	FL_DIR dirstat;
	int filenumber = 0;

	// If first call to library, initialise
	CHECK_FL_INIT();

	FL_LOCK(&_fs);

	FAT_PRINTF(("\r\nNo.             Filename\r\n"));

	if (fl_opendir(path, &dirstat))
	{
		struct fs_dir_ent dirent;

		while (fl_readdir(&dirstat, &dirent) == 0)
		{
			if (dirent.is_dir)
			{
				FAT_PRINTF(("%d - %s <DIR> (0x%08lx)\r\n",++filenumber, dirent.filename, dirent.cluster));
			}
			else
			{
				FAT_PRINTF(("%d - %s [%d bytes] (0x%08lx)\r\n",++filenumber, dirent.filename, dirent.size, dirent.cluster));
			}
		}

		fl_closedir(&dirstat);
	}

	FL_UNLOCK(&_fs);
}
#endif
//-----------------------------------------------------------------------------
// fl_opendir: Opens a directory for listing
//-----------------------------------------------------------------------------
#if FATFS_DIR_LIST_SUPPORT
FL_DIR* fl_opendir(const char* path, FL_DIR *dir)
{
	int levels;
	int res = 1;
	UINT32 cluster = FAT32_INVALID_CLUSTER;

	// If first call to library, initialise
	CHECK_FL_INIT();

	FL_LOCK(&_fs);

	levels = fatfs_total_path_levels((char*)path) + 1;

	// If path is in the root dir
	if (levels == 0)
		cluster = fatfs_get_root_cluster(&_fs);
	// Find parent directory start cluster
	else
		res = _open_directory((char*)path, &cluster);

	if (res)
		fatfs_list_directory_start(&_fs, dir, cluster);

	FL_UNLOCK(&_fs);

	return cluster != FAT32_INVALID_CLUSTER ? dir : 0;
}
#endif
//-----------------------------------------------------------------------------
// fl_readdir: Get next item in directory
//-----------------------------------------------------------------------------
#if FATFS_DIR_LIST_SUPPORT
int fl_readdir(FL_DIR *dirls, fl_dirent *entry)
{
	int res = 0;

	// If first call to library, initialise
	CHECK_FL_INIT();

	FL_LOCK(&_fs);

	res = fatfs_list_directory_next(&_fs, dirls, entry);

	FL_UNLOCK(&_fs);

	return res ? 0 : -1;
}
#endif
//-----------------------------------------------------------------------------
// fl_closedir: Close directory after listing
//-----------------------------------------------------------------------------
#if FATFS_DIR_LIST_SUPPORT
int fl_closedir(FL_DIR* dir)
{
	// Not used
	return 0;
}
#endif
//-----------------------------------------------------------------------------
// fl_is_dir: Is this a directory?
//-----------------------------------------------------------------------------
int fl_is_dir(const char *path)
{
	int res = 0;
	FL_DIR dir;

	if (fl_opendir(path, &dir))
	{
		res = 1;
		fl_closedir(&dir);
	}

	return res;
}
//-----------------------------------------------------------------------------
// fl_get_fs:
//-----------------------------------------------------------------------------
#ifdef FATFS_INC_TEST_HOOKS
struct fatfs* fl_get_fs(void)
{
	return &_fs;
}
#endif
