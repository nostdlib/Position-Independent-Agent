/**
 * @file directory_entry.h
 * @brief Directory entry structure for filesystem iteration
 * @details Defines the DirectoryEntry struct used by DirectoryIterator to
 * represent files and directories. Packed layout with no padding.
 * Part of the PLATFORM layer of the Position-Independent Runtime (PIR).
 */

#pragma once

#include "core/types/primitives.h"

#pragma pack(push, 1)
/// Directory entry structure returned by DirectoryIterator.
struct DirectoryEntry
{
	WCHAR Name[256];         
	UINT64 CreationTime;    
	UINT64 LastModifiedTime; 
	UINT64 Size;             
	UINT32 Type;             
	BOOL IsDirectory;       
	BOOL IsDrive;            
	BOOL IsHidden;          
	BOOL IsSystem;          
	BOOL IsReadOnly;       
	UINT64 VolumeSerial;    
};

#pragma pack(pop)
