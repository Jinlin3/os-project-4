/*
 *  Copyright (C) 2024 CS416/CS518 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

// User-facing file system operations

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here

struct superblock* superblock;
bitmap_t inode_bitmap;
bitmap_t data_block_bitmap;
int inodes_per_block = BLOCK_SIZE / sizeof(struct inode);

/* 
 * Get available inode number from bitmap
 */

void superblock_init() {
	superblock->magic_num = MAGIC_NUM;
	superblock->max_inum = MAX_INUM;
	superblock->max_dnum = MAX_DNUM;
	superblock->i_bitmap_blk = 1;
	superblock->d_bitmap_blk = 2;
	superblock->i_start_blk = 3;
	superblock->d_start_blk = 8;

	bio_write(0, superblock);
}

void inode_bitmap_init() {
	inode_bitmap = malloc(BLOCK_SIZE);
	memset(inode_bitmap, 0, BLOCK_SIZE);
	bio_write(superblock->i_bitmap_blk, inode_bitmap);
}

void data_block_bitmap_init() {
	data_block_bitmap = malloc(BLOCK_SIZE);
	memset(data_block_bitmap, 0, BLOCK_SIZE);
	bio_write(superblock->d_bitmap_blk, data_block_bitmap);
}

// calculates the inode block number
int calc_inode_block_no(int ino_no) {
	int starting_block = superblock->i_start_blk;
	return (starting_block + (ino_no / inodes_per_block));
}

// calculates the byte offset for the specific inode
int calc_inode_offset(int ino_no) {
	return ((ino_no % inodes_per_block) * sizeof(struct inode));
}

void root_inode_init() {
	struct inode root_inode;
	root_inode.ino = 0;
	root_inode.valid = 1;
	root_inode.size = BLOCK_SIZE;
	root_inode.type = S_IFDIR | 0755;
	root_inode.link = 2;
	root_inode.direct_ptr[0] = superblock->d_start_blk;
	// initializing other ptrs to -1 to indicate unused
	for (int i = 1; i < 16; i++) {
		root_inode.direct_ptr[i] = -1;
	}
	for (int i = 0; i < 8; i++) {
		root_inode.indirect_ptr[i] = -1;
	}
	// Writing to disk
	int inode_block_no = calc_inode_block_no(root_inode.ino);
	int inode_offset = calc_inode_offset(root_inode.ino);
	char inode_block[BLOCK_SIZE];
	// Reading block from disk
	bio_read(inode_block_no, inode_block);
	// Modifying the block in memory
	memcpy(inode_block + inode_offset, &root_inode, sizeof(struct inode));
	// Writing block back to disk
	bio_write(inode_block_no, inode_block);
}

// Turns a path into an array of strings
char** parse_path(const char* path) {
	int index = 0; // number of strings in array
	int capacity = 32;
	char** array = malloc(sizeof(char*) * capacity); // array returned
	char string[256]; // temporary string
	int path_ptr = 0;
	int string_ptr = 0;
	// if absolute path, set index 0 to "/"
	if (path[0] == '/') {
		array[index] = strdup("/");
		index++;
		path_ptr++;
	}

	// Iterate through path string
	while(path[path_ptr] != '\0') {
		if (path[path_ptr] == '/') { // if '/', you reached the end of the string
			string[string_ptr] = '\0'; // null terminate the string
			array[index] = strdup(string);
			string_ptr = 0; // reset inner pointer
			index++;
		} else { // else, copy the character into temp
			string[string_ptr] = path[path_ptr];
			string_ptr++;
		}
		path_ptr++;
	}
	// If the path doesn't end in a null terminator, add the last string
	if (string_ptr > 0) {
		string[string_ptr] = '\0';
		array[index] = strdup(string);
		index++;
	}
	array[index] = NULL;
	return array;
}

// returns the length of a string
int string_len(char* string) {
	int length = 0;
	for (int i = 0; string[i] != '\0'; i++) {
		length++;
	}
	return length;
}

// returns the length of an array (assuming it is null terminated)
int array_len(char** array) {
	int length = 0;
	for (int i = 0; array[i] != NULL; i++) {
		length++;
	}
	return length;
}

// frees the array from parse_path()
int free_array(char** array) {
	for (int i = 0; array[i] != NULL; i++) {
		free(array[i]);
	}
	free(array);
	return 0;
}

int get_avail_ino() {
	// Step 1: Read inode bitmap from disk
	bio_read(superblock->i_bitmap_blk, inode_bitmap);
	// Step 2: Traverse inode bitmap to find an available slot
	int available_slot = -1;
	for (int i = 0; i < superblock->max_inum; i++) {
		if (get_bitmap(inode_bitmap, i) == 0) {
			available_slot = i;
			set_bitmap(inode_bitmap, i);
			break;
		}
	}
	if (available_slot == -1) {
		printf("No available inodes.\n");
	}
	// Step 3: Update inode bitmap and write to disk 
	bio_write(superblock->i_bitmap_blk, inode_bitmap);
	return available_slot;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {
	// Step 1: Read data block bitmap from disk
	bio_read(superblock->d_bitmap_blk, data_block_bitmap);
	// Step 2: Traverse data block bitmap to find an available slot
	int available_slot = -1;
	for (int i = 0; i < superblock->max_dnum; i++) {
		if (get_bitmap(data_block_bitmap, i) == 0) {
			available_slot = i;
			set_bitmap(data_block_bitmap, i);
			break;
		}
	}
	if (available_slot == -1) {
		printf("No available data blocks.\n");
	}
	// Step 3: Update data block bitmap and write to disk
	bio_write(superblock->d_bitmap_blk, data_block_bitmap);
	return available_slot;
}

/* 
 * inode operations
 */

// Reads corresponding on-disk inode to an in-memory inode
int readi(uint16_t ino, struct inode *inode) {
  // Step 1: Get the inode's on-disk block number
	int block_no = calc_inode_block_no(ino);
  // Step 2: Get offset of the inode in the inode on-disk block
	int offset = calc_inode_offset(ino);
  // Step 3: Read the block from disk and then copy into inode structure
	char block[BLOCK_SIZE];
	bio_read(block_no, block);
	memcpy(inode, block + offset, sizeof(struct inode));

	return 0;
}

// Writes the in-memory inode to the disk inode
int writei(uint16_t ino, struct inode *inode) {
	// Step 1: Get the block number where this inode resides on disk
	int block_no = calc_inode_block_no(ino);
	// Step 2: Get the offset in the block where this inode resides on disk
	int offset = calc_inode_offset(ino);
	// Step 3: Write inode to disk 
	char block[BLOCK_SIZE];
	bio_read(block_no, block);
	memcpy(block + offset, inode, sizeof(struct inode));
	bio_write(block_no, block);

	return 0;
}


/* 
 * directory operations
 */
// Returns -1 if directory doesn't exist
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);
  // Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode* directory_inode = malloc(sizeof(struct inode));
	readi(ino, directory_inode);
  // Step 2: Get data block of current directory from inode
	char block[BLOCK_SIZE];

	for (int i = 0; i < 16; i++) {
		int data_block_ptr = directory_inode->direct_ptr[i];
		if (data_block_ptr != -1) {
			bio_read(data_block_ptr, block);
			// Step 3: Read directory's data block and check each directory entry.
			// If the name matches, then copy directory entry to dirent structure
			struct dirent* entry = (struct dirent*)block;
			for (int j = 0; j < entries_per_block; j++) {
				if (entry[j].valid && strcmp(fname, entry[j].name) == 0) {
					memcpy(dirent, &entry[j], sizeof(struct dirent));
					return 0;
				}
			}
		}
	}

	free(directory_inode);
	printf("No dirent found!\n");
	return -1;
}

// Writes a new directory entry into the current directory's data blocks
int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	// Step 3: Add directory entry in dir_inode's data block and write to disk
	// Allocate a new data block for this directory if it does not exist
	// Update directory inode
	// Write directory entry

	// Variables
	int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);
	char data_block[BLOCK_SIZE];
	int entry_added = 0;

	// Checking to see if a directory already exists with the same name
	struct dirent existing_entry;
	if (dir_find(dir_inode.ino, fname, name_len, &existing_entry) == 0) {
		printf("Directory already exists.\n");
		return -1;
	}

	// Looking for existing memory block ; only memory block needs to be written to disk
	for (int i = 0; i < 16; i++) {
		if (dir_inode.direct_ptr[i] != -1) { // if direct ptr exists
			bio_read(dir_inode.direct_ptr[i], data_block); // read data block to memory
			struct dirent* entry = (struct dirent*)data_block; // preparing to read entries
			for (int j = 0; j < entries_per_block; j++) {
				if (!entry[j].valid) {
					// setting entry
					entry[j].ino = f_ino;
					entry[j].valid = 1;
					strncpy(entry[j].name, fname, name_len);
					entry[j].len = name_len;
					// writing data block to disk
					bio_write(dir_inode.direct_ptr[i], data_block);
					entry_added = 1;
					break;
				}
			}
		} else { // if direct ptr does not exist, initialize it and add the entry there
			int new_block_no = get_avail_blkno();
			if (new_block_no == -1) {
				printf("No available blocks on disk.\n");
				return -1;
			}
			char new_block[BLOCK_SIZE];
			memset(new_block, 0, BLOCK_SIZE);
			struct dirent* entry = (struct dirent*)new_block;

			entry[0].ino = f_ino;
			entry[0].valid = 1;
			strncpy(entry[0].name, fname, name_len);
			entry[0].len = name_len;

			dir_inode.direct_ptr[i] = new_block_no;
			bio_write(new_block_no, new_block); // write new block to disk
			entry_added = 1;
			break;
		}
	}
	// Return 0 if success, -1 otherwise
	if (entry_added == 1) {
		writei(dir_inode.ino, &dir_inode); // write updated inode to disk
		return 0;
	} else {
		return -1;
	}
}

// Required for 518
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

// Returns -1 if directory is missing
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	char** array = parse_path(path);
	if (!array) {
		printf("Memory allocation error for array.\n");
		return -1;
	}

	// Implementation
	struct dirent* dirent = malloc(sizeof(struct dirent));
	struct inode* current_inode = malloc(sizeof(struct inode));
	int name_len;
	readi(ino, current_inode); // reads root inode
	for (int i = 1; array[i] != NULL; i++) {
		name_len = string_len(array[i]);
		if (dir_find(current_inode->ino, array[i], name_len, dirent) == -1) {
			printf("Directory is missing.\n");
			return -1;
		}
		readi(dirent->ino, current_inode);
	}
	*inode = *current_inode;
	printf("Retrieved node %d by path.\n", current_inode->ino);
	free(dirent);
	free(current_inode);
	free_array(array);

	return 0;
}

/* 
 * Make file system
 */
int rufs_mkfs() {
	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);
	// write superblock information
	superblock_init();
	// initialize inode bitmap
	inode_bitmap_init();
	// initialize data block bitmap
	data_block_bitmap_init();
	// update bitmap information for root directory
	set_bitmap(inode_bitmap, 0);
	bio_write(superblock->i_bitmap_blk, inode_bitmap);
	set_bitmap(data_block_bitmap, 0);
	bio_write(superblock->d_bitmap_blk, data_block_bitmap);
	// update inode for root directory
	root_inode_init();
	return 0;
}


/* 
 * FUSE file operations
 */
static void* rufs_init(struct fuse_conn_info *conn) {
	superblock = malloc(sizeof(struct superblock));
	// Step 1a: If disk file is not found, call mkfs
	if (dev_open(diskfile_path) == -1) {
		printf("Disk file not found. Formatting disk...\n");
		rufs_mkfs();
	} else {
	// Step 1b: If disk file is found, just initialize in-memory data structures and read superblock from disk
		bio_read(0, superblock);
	}
	printf("RUFS initialized.\n");
	return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures
	free(superblock);
	free(inode_bitmap);
	free(data_block_bitmap);
	// Step 2: Close diskfile
	dev_close();

}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path
	struct inode *inode = malloc(sizeof(struct inode));
	if (get_node_by_path(path, 0, inode) == -1) {
		printf("Invalid path.\n");
		return -ENOENT;
	}
	// Step 2: fill attribute of file into stbuf from inode
	if (inode->ino == 0) { // if root directory
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else { // if other directories
		stbuf->st_mode = inode->type | 0755;
		stbuf->st_nlink = inode->link;
	}
	stbuf->st_atime = inode->vstat.st_atime;
	stbuf->st_blksize = inode->vstat.st_blksize;
	stbuf->st_blocks = inode->vstat.st_blocks;
	stbuf->st_ctime = inode->vstat.st_ctime;
	stbuf->st_dev = inode->vstat.st_dev;
	stbuf->st_gid = getgid();
	stbuf->st_ino = inode->vstat.st_ino;
	stbuf->st_mtime = inode->vstat.st_mtime;
	stbuf->st_rdev = inode->vstat.st_rdev;
	stbuf->st_size = inode->size;
	stbuf->st_uid = getuid();
	
	free(inode);

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode inode;
	// Step 2: If not find, return -1
	if (get_node_by_path(path, 0, &inode) == -1) {
		printf("Invalid path.\n");
		return -1;
	} else {
		return 0;
	}
}

// ls command
static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	printf("--LS--\n");
	// Step 1: Call get_node_by_path() to get inode from path
	struct inode inode;
	if (get_node_by_path(path, 0, &inode) == -1) {
		printf("Invalid path.\n");
		return -1;
	}
	// Step 2: Read directory entries from its data blocks, and copy them to filler
	int entries_per_block = BLOCK_SIZE / sizeof(struct dirent);
	char block[BLOCK_SIZE];

	for (int i = 0; i < 16; i++) {
		if (inode.direct_ptr[i] != -1) {
			bio_read(inode.direct_ptr[i], block);
			struct dirent* entry = (struct dirent*)block;
			for (int j = 0; j < entries_per_block; j++) {
				if (entry->valid) {
					filler(buffer, entry->name, NULL, 0);
				}
				entry++;
			}
		}
	}
	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	// Duplicate path to avoid modifications by dirname and basename
	printf("--RUFS_MKDIR--\n");
  char *path_dup = strdup(path);
  char *path_base = strdup(path);
  char *directory_name = dirname(path_dup);
  char *base_name = basename(path_base);
	printf("DIRECTORY NAME: %s\n", directory_name);
	printf("BASE_NAME: %s\n", base_name);
	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode inode;
	if (get_node_by_path(directory_name, 0, &inode) == -1) {
		printf("Path invalid.\n");
		free(path_dup);
		free(path_base);
		return -1;
	}
	// get an available inode number
	int available_inode_no = get_avail_ino();
	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	struct dirent dirent;
	if (dir_find(inode.ino, base_name, strlen(base_name), &dirent) == 0) {
		printf("Can't create because directory already exists.\n");
		free(path_dup);
		free(path_base);
		return -1;
	}
	// Add new directory entry
	dir_add(inode, available_inode_no, base_name, strlen(base_name));
	// Step 6: Call writei() to write inode to disk
	struct inode* new_inode = malloc(sizeof(struct inode));
	new_inode->ino = available_inode_no;
	new_inode->valid = 1;
	new_inode->size = BLOCK_SIZE;
	new_inode->type = S_IFDIR | 0755;
	new_inode->link = 2;
	new_inode->direct_ptr[0] = get_avail_blkno();
	
	for (int i = 1; i < 16; i++) {
		new_inode->direct_ptr[i] = -1;
	}
	for (int i = 0; i < 8; i++) {
		new_inode->indirect_ptr[i] = -1;
	}
	writei(available_inode_no, new_inode);
	free(new_inode);
	free(path_dup);
	free(path_base);

	return 0;
}

// Required for 518
static int rufs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
  // For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	char* path_dup = strdup(path);
	char* path_base = strdup(path);
	char* directory_name = dirname(path_dup);
	char* base_name = basename(path_base);
	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode inode;
	if (get_node_by_path(directory_name, 0, &inode) == -1) {
		printf("Path invalid.\n");
		free(path_dup);
		free(path_base);
		return -1;
	}
	// Step 3: Call get_avail_ino() to get an available inode number
	int available_inode_no = get_avail_ino();
	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	struct dirent dirent;
	if (dir_find(inode.ino, base_name, strlen(base_name), &dirent) == 0) {
		printf("File already exists.\n");
		free(path_dup);
		free(path_base);
		return -1;
	}
	// Step 5: Update inode for target file
	// Add new directory entry
	dir_add(inode, available_inode_no, base_name, strlen(base_name));
	// Step 6: Call writei() to write inode to disk
	struct inode* new_inode = malloc(sizeof(struct inode));
	new_inode->ino = available_inode_no;
	new_inode->valid = 1;
	new_inode->size = BLOCK_SIZE;
	new_inode->type = S_IFREG | 0644;
	new_inode->link = 1;
	new_inode->direct_ptr[0] = get_avail_blkno();
	
	for (int i = 1; i < 16; i++) {
		new_inode->direct_ptr[i] = -1;
	}
	for (int i = 0; i < 8; i++) {
		new_inode->indirect_ptr[i] = -1;
	}
	writei(available_inode_no, new_inode);
	free(new_inode);
	free(path_dup);
	free(path_base);
	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1
	struct inode inode;
	if (get_node_by_path(path, 0, &inode) == -1) {
		printf("Failed to open file.\n");
		return -1;
	}
	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode inode;
	get_node_by_path(path, 0, &inode);
	int bytes_read = 0; // total bytes read
	int bytes_to_read = 0; // bytes to read in block
	int bytes_remaining = size; // total bytes remaining to read
	int block_offset = 0;
	int start_block = offset / BLOCK_SIZE;
	int end_block = (offset + size) / BLOCK_SIZE;
	char block[BLOCK_SIZE];
	// Step 2: Based on size and offset, read its data blocks from disk
	for (int i = start_block; i <= end_block; i++) {
		if (inode.direct_ptr[i] != -1) {
			bio_read(inode.direct_ptr[i], block);
			if (bytes_read == 0) { // if starting
				block_offset = offset % BLOCK_SIZE;
			} else {
				block_offset = 0;
			}
			if (bytes_remaining < BLOCK_SIZE) {
				bytes_to_read = bytes_remaining;
			} else {
				bytes_to_read = BLOCK_SIZE - block_offset;
			}
			memcpy(buffer + bytes_read, block + block_offset, bytes_to_read);
			bytes_read += bytes_to_read;
			bytes_remaining -= bytes_to_read;
		}
	}
	return bytes_read;
	// Step 3: copy the correct amount of data from offset to buffer
	// Note: this function should return the amount of bytes you copied to buffer

}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path
	// Step 2: Based on size and offset, read its data blocks from disk
	// Step 3: Write the correct amount of data from offset to disk
	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode inode;
	get_node_by_path(path, 0, &inode);
	int bytes_written = 0; // total bytes read
	int bytes_to_write = 0; // bytes to read in block
	int bytes_remaining = size; // total bytes remaining to read
	int block_offset = 0;
	int start_block = offset / BLOCK_SIZE;
	int end_block = (offset + size) / BLOCK_SIZE;
	char block[BLOCK_SIZE];
	// Step 2: Based on size and offset, read its data blocks from disk
	for (int i = start_block; i <= end_block; i++) {
		if (inode.direct_ptr[i] != -1) {
			bio_read(inode.direct_ptr[i], block);
			if (bytes_written == 0) { // if starting
				block_offset = offset % BLOCK_SIZE;
			} else {
				block_offset = 0;
			}
			if (bytes_remaining < BLOCK_SIZE) {
				bytes_to_write = bytes_remaining;
			} else {
				bytes_to_write = BLOCK_SIZE - block_offset;
			}
			memcpy(block + block_offset, buffer + bytes_written, bytes_to_write);
			bytes_written += bytes_to_write;
			bytes_remaining -= bytes_to_write;
			bio_write(inode.direct_ptr[i], block);
		}
	}
	return bytes_written;
	// Step 3: copy the correct amount of data from offset to buffer
	// Note: this function should return the amount of bytes you copied to buffer
}

// Required for 518

static int rufs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

