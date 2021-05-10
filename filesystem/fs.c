#include "fs.h"

#ifdef LINUX_SIM
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#endif /* LINUX_SIM */

#include "common.h"
#include "block.h"
#include "util.h"
#include "thread.h"
#include "inode.h"
#include "superblock.h"
#include "kernel.h"
#include "fs_error.h"

#define BITMAP_ENTRIES 256

#define INODE_TABLE_ENTRIES 20

#define INODE_BLOCKS 32
#define BITMAP_BLOCKS 2
#define INODE_SIZE 32
#define MAX_INODES 512

static char inode_bmap[BITMAP_ENTRIES];
static char dblk_bmap[BITMAP_ENTRIES];

static int get_free_entry(unsigned char *bitmap);
static int free_bitmap_entry(int entry, unsigned char *bitmap);
static inode_t name2inode(char *name);
static blknum_t ino2blk(inode_t ino);
static blknum_t idx2blk(int index);

static uint32_t SUPER_BLOCK_START = 0;

static struct mem_inode inodes[MAX_INODES];
struct disk_superblock super;


/* Writes the 2 bitmaps to drive */
void save_bitmaps() {
    block_modify(SUPER_BLOCK_START + 1, 0, inode_bmap, BITMAP_ENTRIES);
    block_modify(SUPER_BLOCK_START + 2, 0, dblk_bmap, BITMAP_ENTRIES);
}
/* Loads the 2 bitmaps from drive */
void load_bitmaps() {
    block_read_part(SUPER_BLOCK_START + 1, 0, BITMAP_ENTRIES, inode_bmap);
    block_read_part(SUPER_BLOCK_START + 2, 0, BITMAP_ENTRIES, dblk_bmap);
}
/* Counts how many entries are used in the bitmap */
int bitmap_used_space(char* bitmap) {
    int counter = 0;
    for(int x = 0; x < BITMAP_ENTRIES; x++) {
        for(int y = 0; y < 8; y++) {
            if((bitmap[x] & (1 << y)) > 0) {
                counter++;
            }
        }
    }
    return counter;
}
/* Writes out number of inodes and datablocks in use
 * usefully for checking that creating and deleting inodes and data is correct
 */
void print_debug_info() {
    load_bitmaps();

    scrprintf(0,0,"Inodes in use: %i\n", bitmap_used_space(inode_bmap));
    scrprintf(1,0,"Datablocks in use: %i\n", bitmap_used_space(dblk_bmap));
}


/* saves inode to drive */
void save_inode(inode_t id) {
    int iblock = ino2blk(id);
    block_modify(iblock, (id % 16) * 32, &inodes[id].d_inode, sizeof(inodes[id].d_inode));
}

int check_bit(int i, char* bitmap) {
        int index = i / 8;
        int bit = 7-(i % 8);
        if((bitmap[index] & 1 << bit) > 0) {
            return 1;
        }
        return 0;
}

/* loads a inode from drive */
int load_inode(inode_t id) {
    int iblock = ino2blk(id);
    block_read_part(iblock, (id % 16) * 32, sizeof(inodes[id].d_inode), &inodes[id].d_inode);

    int inode_size = inodes[id].d_inode.size;
    if(inode_size > super.max_filesize) { // Corrupted inode
        return FSE_ERROR;
    }
    for(int x = 0; x < inode_size; x+= BLOCK_SIZE) {
        if(inodes[id].d_inode.direct[x] < 0 || !check_bit(inodes[id].d_inode.direct[x], dblk_bmap)) {
            return FSE_ERROR; // Corrupted inode, size does not match datablocks
        }
    }
    inodes[id].open_count = 0;
    inodes[id].pos = 0;
    inodes[id].dirty = 0;
    inodes[id].inode_num = id;
    return FSE_OK;
}
/* resizes a inode */
int resize_inode(inode_t id, int new_size) {
    if(new_size > super.max_filesize) {
        return FSE_INODETABLEFULL; // What to return here? inode to big.
    }
    load_bitmaps();
    struct mem_inode *i = &inodes[id];
    int blocks = (new_size / BLOCK_SIZE) + 1;
    for(int x = 0; x < INODE_NDIRECT; x++) {
        if(x < blocks) {
            if(i->d_inode.direct[x] == -1) {
                i->d_inode.direct[x] = get_free_entry(dblk_bmap);
                if(i->d_inode.direct[x] == -1 || i->d_inode.direct[x] >= super.ndata_blks) { // Not enought free space
                    return FSE_FULL;             // Return without allocating anything
                }
            }
        }else {
            if(i->d_inode.direct[x] != -1) {
                free_bitmap_entry(i->d_inode.direct[x], dblk_bmap);
            }
        }
    }
    i->d_inode.size = new_size;
    save_bitmaps();
    save_inode(id);
    return FSE_OK;
}

/* Dont call this, call create_directory or create_file, does not save to disk by itself */
int create_inode() {
    int i = get_free_entry(inode_bmap);
    if(i < 0 || i >= MAX_INODES)
        return FSE_NOMOREINODES;
    save_bitmaps();
    struct disk_inode *dnode = &inodes[i].d_inode;
    dnode->type = INTYPE_FILE;
    dnode->size = 0;
    dnode->nlinks = 0;
    for(int x = 0; x < INODE_NDIRECT; x++) {
        dnode->direct[x] = -1;
    }
    inodes[i].open_count = 0; // incr / dec it when a file is open/closed
    inodes[i].pos = 0;
    inodes[i].dirty = 1;
    inodes[i].inode_num = i;
    return i;
}
/* Frees a inode and the datablocks it links to */
void free_inode(int id) {
    struct disk_inode *dnode = &inodes[id].d_inode;
    for(int x = 0; x < INODE_NDIRECT; x++) {
        if(dnode->direct[x] != -1) {
            free_bitmap_entry(dnode->direct[x], dblk_bmap);
        }
    }
    free_bitmap_entry(id, inode_bmap);
    save_bitmaps();
}
/* Reduces the nlinks of a inode, if its 0 or below left deletes the inode */
void reduce_links(inode_t id) {
    struct disk_inode *dnode = &inodes[id].d_inode;
    dnode->nlinks--;
    if(dnode->nlinks <= 0 || dnode->type == INTYPE_DIR) {
        free_inode(id);
    }else {
        save_inode(id);
    }
}

/* Reads from inode datablocks
 * params:
 *   inode_t id : the inode the datablocks belongs to
 *   char* buffer : pointer to the data to read to
 *   int size : how much data to be read
 *   int start_pos : offset to start reading from
 * returns how much was read or error msg
 */
int db_read(inode_t id, char* buffer, int size, int start_pos) {
    struct mem_inode *i = &inodes[id];
    int finish_pos = size + start_pos;
    if(finish_pos > i->d_inode.size) { // Only read up to the size of the inode
        finish_pos = i->d_inode.size; // Could also possibly return a error message instead
    }
    int start_block = start_pos / BLOCK_SIZE;
    int finish_block = (finish_pos / BLOCK_SIZE) + 1;
    int toread = finish_pos;
    if(toread < 0) { //
        return FSE_ERROR;
    }
    int read = 0;
    for(int x = start_block; x < finish_block && (read+start_pos) < i->d_inode.size; x++) {
        // I hope i accounted for every situation here
        if(x == start_block) {
            int in = BLOCK_SIZE - (start_pos % BLOCK_SIZE);
            if(x+1 == finish_block) {
                in = finish_pos-start_pos;
            }
            block_read_part(idx2blk(i->d_inode.direct[x]), start_pos % BLOCK_SIZE, in, &buffer[read]);
            read += in;
        }else if(x+1 == finish_block) {
            int in = (finish_pos-start_pos) - read;
            block_read_part(idx2blk(i->d_inode.direct[x]), 0, in, &buffer[read]);
            read += in;
        }
        else {
            block_read_part(idx2blk(i->d_inode.direct[x]), 0, BLOCK_SIZE, &buffer[read]);
            read += BLOCK_SIZE;
        }
    }
    return read;
}
/* Writes to inode datablocks (Its almost the same as db_read and i coulda probably merged those functions)
 * returns the amount written
 * params:
 *   inode_t id : the inode the datablocks belongs to
 *   char* buffer : pointer to the data to write to file
 *   int size : how much data to be written
 *   int start_pos : offset to start writing from
 * returns how much was written or error msg
 */
int db_write(inode_t id, char* buffer,int size, int start_pos) {
    struct mem_inode *i = &inodes[id];
    int start_block = start_pos / BLOCK_SIZE;
   // Resize file if nessesary
    int finish_pos = size+start_pos;
    int finish_block = (finish_pos / BLOCK_SIZE) + 1;
    if(finish_pos > super.max_filesize) { // If we extend the max filesize only write up to max filesize
        finish_pos = super.max_filesize;  // Could also possibly return a error message instead
    }
    int resize = resize_inode(id,finish_pos);
    if(resize != FSE_OK) {
        return resize;
    }
    int written = 0;
    for(int x = start_block; x < finish_block && start_pos + written < i->d_inode.size; x++) {
        // Do we have less than 1 block left to write? if not fill up the block
        if (x == start_block) {
            int in = BLOCK_SIZE - (start_pos % BLOCK_SIZE);
            if(x+1 == finish_block) {
                in = finish_pos-start_pos;
            }
            block_modify(idx2blk(i->d_inode.direct[x]), start_pos % BLOCK_SIZE, &buffer[written], in);
            written += in;
        }else if(x+1 == finish_block) {
            int in = (finish_pos-start_pos) - written;
            block_modify(idx2blk(i->d_inode.direct[x]), 0, &buffer[written], in);
            written += in;
        }
        else {
            block_modify(idx2blk(i->d_inode.direct[x]), 0, &buffer[written], BLOCK_SIZE);
            written += BLOCK_SIZE;
        }
    }
    return written;
}

/* Adds a entry to a directory
 *
 */
int create_directory_entry(int dir, int inode, char* name) {

    int len = strlen(name) + 1;
    if(len > MAX_FILENAME_LEN) {
        len = MAX_FILENAME_LEN;
    }
    struct dirent entry;
    bcopy(name, entry.name, len);
    entry.name[len-1] = '\0';
    entry.inode = inode;

    struct disk_inode *dnode = &inodes[dir].d_inode;

    int r = resize_inode(dir, dnode->size + sizeof(struct dirent));
    if(r != FSE_OK) {
        return r;
    }
    r = db_write(dir, (char*)&entry, sizeof(struct dirent), dnode->size - sizeof(struct dirent));
    if(r < 0) {
        return r;
    }
    struct disk_inode *fnode = &inodes[inode].d_inode;
    fnode->nlinks++;
    save_inode(inode);
    return FSE_OK;
}
/* Removes the first occurence of the entry in a directory
 * Assuming directories can only exist in one place since hardlinking them is not allowed
 * Does not check if the entry exists, doing this in the fs_ functions
 * Will delete the file if its the last reference to it
 */
int remove_directory_entry(int dir, inode_t id) {
    // If the entry is another directory we need to clean up inside that directory before we can remove it
    if(inodes[id].d_inode.type == INTYPE_DIR) {
        int entries = inodes[id].d_inode.size / sizeof(struct dirent);
        struct dirent buffer[entries];
        int read = db_read(id, buffer, inodes[id].d_inode.size, 0);
        for(int x = 0; x < entries; x++) {
            if(buffer[x].inode != dir && buffer[x].inode != id) { // Prevent entering "." and ".." as this will cause us to loop, instead just remove them below
                remove_directory_entry(id, buffer[x].inode); // Recursive deleting
            }
        }
    }
    // Removes the entry from the directory
    // Because of the way ls works we have to restructure the data blocks for the inode
    // If we remove directories inside directories we get some unnessesary writing to drive here but optimizing all that is gona take me to long
    int entries = inodes[dir].d_inode.size / sizeof(struct dirent);
    struct dirent buffer[entries];
    int read = db_read(dir, (char*)buffer, inodes[dir].d_inode.size,0);
    struct dirent newBuffer[entries-1];
    int j = 0;
    int found = 0;
    for(int x = 0; x < entries; x++) {
        if(buffer[x].inode == id && found < 1) {
            found++;
            reduce_links(id);
        } else {
            newBuffer[j] = buffer[x];
            j++;
        }
    }
    // Theres no reason this should fail since we are keeping or shrinking the amount of datablocks
    int new_size = inodes[dir].d_inode.size - (sizeof(struct dirent)); // * found);
    resize_inode(dir, new_size);
    db_write(dir, (char*)newBuffer, new_size, 0);
    return FSE_OK;
}
/* creates a directory with self "." entry and parent ".." entry
 *
 */
int create_directory(int parent) {
    int dir = create_inode();
    if(parent == -1) { // Incase of root inode parent is itself
        parent = dir;
    }
    if(dir < 0)
        return FSE_NOMOREINODES; // Couldnt create a inode
    struct disk_inode *dnode = &inodes[dir].d_inode;
    dnode->type = INTYPE_DIR;
    int i = create_directory_entry(dir, dir, ".") + create_directory_entry(dir, parent, "..");
    // if we failed to create a directory with 2 entries, delete the inode
    if(i != FSE_OK) {
        free_inode(dir);
        return FSE_FULL;
    }
    save_inode(dir);

    return dir;
}

/* Creates a new file in the current directory
 * with the given name
 * Should only be called after checking that the file does not already exist
 */
int create_file(int dir, char* filename) {
    inode_t file = create_inode();
    if(file < 0) {
        return file;
    }
    int en = create_directory_entry(dir, file, filename);
    if(en != FSE_OK) {
        free_inode(file);
        return en;
    }
    save_inode(file);
    save_bitmaps();
    return file;
}



/*
 * This function is called by the "loader_thread" after USB
 * subsystem has been initialized.
 *
 * It should check whether there is a filesystem on the
 * disk and perform the necessary operations to prepare it
 * for usage.
 */
void fs_init(void)
{


    block_init();

    SUPER_BLOCK_START = 2 + os_size; // Boot + os

    // I only check if the values in super block looks correctly
    // in a proper file system you probably want to check everything for corruption
    // Theres no way to guarantee that theres actually a file system on the disk or just random data
    // that happens to line up perfectly with the system (tho the chance for that is probably extremly low)

    block_read_part(SUPER_BLOCK_START,0, sizeof(super), &super);
    if(super.ninodes != MAX_INODES
       || super.ndata_blks != FS_BLOCKS - INODE_BLOCKS - BITMAP_BLOCKS - 1
       || super.max_filesize != 4096
       ) {
        fs_mkfs();
    }
    else {
        load_bitmaps();

        for(int x = 0; x < MAX_INODES; x++) {
            int index = x / 8;
            int bit = 7-(x % 8);
            if((inode_bmap[index] & 1 << bit) > 0) {
                int i = load_inode(x);
                // If a inode is corrupted free it and write a error msg.
                // Not attempting data-recovery in this assigment
                if(i != FSE_OK) {
                    scrprintf(4,0,"Corrupted inode detected\n");
                    free_inode(x);
                }
            }
        }
        //print_debug_info();
    }
}

/*
 * Make a new file system.
 *
 * The kernel_size is passed to _start(..) in kernel.c by
 * the bootloader.
 */
void fs_mkfs(void)
{
    bzero(inode_bmap, BITMAP_ENTRIES);
    bzero(dblk_bmap, BITMAP_ENTRIES);
    save_bitmaps();

    super.ninodes = 512;
    super.ndata_blks = FS_BLOCKS - INODE_BLOCKS - BITMAP_BLOCKS - 1;
    super.max_filesize = 4096;
    int root = create_directory(-1);
    if(root < 0) {
        scrprintf(0,0,"COULD NOT CREATE ROOT DIRECTORY\n");
        return;
    }
    super.root_inode = root;
    block_modify(SUPER_BLOCK_START, 0, &super, sizeof(super));

    // Testing code

    /* print_debug_info(); */
    /* int doc = create_directory(root); */
    /* create_directory_entry(root, doc, "Documents"); */
    /* int pro = create_directory(doc); */
    /* create_directory_entry(doc, pro, "Programs"); */
    /* int file = create_inode(); */
    /* create_directory_entry(pro, file, "hello.world"); */
    /* int pic = create_directory(root); */
    /* create_directory_entry(root, pic, "Pictures"); */

    /* print_debug_info(); */

    //int t = create_file(root, "txt");

    // This might not work with every compiler
    //char data[1024] = {[0 ... 1023] = 'x'};

    /* for(int x = 0; x < super.max_filesize+10; x+=1024) { */
    /*     char a = 'a'; */
    /*     db_write(t, data, 1024, x); */
    /* } */

    /* print_debug_info(); */

    /* fs_unlink("txt"); */
    /* print_debug_info(); */
}

static inode_t name2inode_f(int dir, char *name);

/* Opens a file, must be called before a file descriptor can be used
 * returns errors if the file could not be opened
 */
int fs_open(const char *filename, int mode)
{
    if(current_running->cwd <= 0) {
        current_running->cwd = super.root_inode;
    }
    int retval = FSE_OK;
    int i = -1;
    int x = 0;
    for(x = 0; x < MAX_OPEN_FILES; x++) {
        if(current_running->filedes[x].mode == MODE_UNUSED) {
            if(filename[0] == '/') {
                i = current_running->cwd;
            }
            else {
                i = name2inode_f(current_running->cwd, filename);
                if(i < 0) {
                    if((mode & MODE_CREAT) != 0) {
                        i = create_file(current_running->cwd, filename);
                        if(i < 0) {
                            retval = i;
                        }
                    }
                    else {
                        retval = FSE_NOTEXIST;
                    }
                }
            }
            break;
        }
    }
    if (retval == FSE_OK) {
        current_running->filedes[x].mode = mode;
        current_running->filedes[x].idx = i;
        inodes[i].pos = 0;
        inodes[i].open_count++;
    }
    return retval;
}
/* Closes the file descriptor */
int fs_close(int fd)
{
    if(current_running->filedes[fd].mode == MODE_UNUSED) {
        return FSE_OK; // Not really a error / problem
    }
    int id = current_running->filedes[fd].idx;
    struct mem_inode *i = &inodes[id];
    i->pos = 0;
    i->open_count--;
    current_running->filedes[fd].mode = MODE_UNUSED;
    current_running->filedes[fd].idx = -1;
    return FSE_OK;
}
/* Reads from file descriptor into buffer
 * returns the result from reading
 * Note: Theres no protection against buffer overflow if size > buffer size
 */
int fs_read(int fd, char *buffer, int size)
{
    // This should also guarantee that we are not in MODE_UNUSED
    if((current_running->filedes[fd].mode & (MODE_RDONLY | MODE_RDWR )) == 0) {
        return FSE_INVALIDMODE;
    }
    int id = current_running->filedes[fd].idx;
    int read = db_read(id, buffer, size, inodes[id].pos);
    if(read < 0) {
        return read;
    }
    int seek = fs_lseek(fd, read, SEEK_CUR);
    if(seek != FSE_OK) {
        return seek;
    }
    return read;
}
/* Writes buffer to file descriptor
 * returns the result of writing
 */
int fs_write(int fd, char *buffer, int size)
{
    if((current_running->filedes[fd].mode & (MODE_WRONLY | MODE_RDWR )) == 0 ) {
        return FSE_INVALIDMODE;
    }

    int id = current_running->filedes[fd].idx;
    int written = db_write(id, buffer, size, inodes[id].pos);
    if(written < 0) {
        return written;
    }
    int seek = fs_lseek(fd, written, SEEK_CUR);
    if(seek != FSE_OK) {
        return seek;
    }
    return written;
}


/*
 * fs_lseek:
 * This function is really incorrectly named, since neither its offset
 * argument or its return value are longs (or off_t's). Also, it will
 * cause blocks to allocated if it extends the file (holes are not
 * supported in this simple filesystem).
 */
int fs_lseek(int fd, int offset, int whence)
{
    if(current_running->filedes[fd].mode == MODE_UNUSED) {
        return FSE_INVALIDMODE;
    }
    int id = current_running->filedes[fd].idx;
    struct mem_inode *i = &inodes[id];

    int pos = offset;
    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        pos += i->pos;
        break;
    case SEEK_END:
        pos += i->d_inode.size;
        break;
    default:
        return FSE_INVALIDMODE;
    }
    if(pos > i->d_inode.size) {
        // If we are in read only dont extend the file size
        if((current_running->filedes[fd].mode & MODE_RDONLY) > 0 ) {
            return FSE_EOF;
        }
        // Dont make file bigger than what we support
        else if(pos > super.max_filesize) {
            return FSE_FULL;
        }
        // Allocate new blocks if needed
        else {
            if(!resize_inode(id, offset)) {
                return FSE_FULL;
            }
        }
    }
    i->pos = pos;
    return FSE_OK;
}
/* Creates a directory if possible
 * returns the result of creation
 */
int fs_mkdir(char *dirname)
{
    if(current_running->cwd <= 0) {
        current_running->cwd = super.root_inode;
    }
    int dir = create_directory(current_running->cwd);
    if(dir >= 0) {
        if(create_directory_entry(current_running->cwd, dir, dirname) != FSE_OK) {
            free_inode(dir);
            return FSE_FULL;
        }
        return FSE_OK;
    }
    return FSE_NOMOREINODES;
}
/* Changes working dir to the path given if possible */
int fs_chdir(char *path)
{
    int id = name2inode(path);
    if(id < 0) { // Not valid path
        return FSE_NOTEXIST;
    }
    if(inodes[id].d_inode.type == INTYPE_DIR) { // Path is dir
        current_running->cwd = id;
        return FSE_OK;
    }
    return FSE_DIRISFILE; // Path is a file
}
/* Deletes a directory, if the directory contains files or other directories it will also delete / reduce nlinks to those
 * returns OK if it was a success or errors if the directory was invalid
 */
int fs_rmdir(char *path)
{
    char remove[MAX_FILENAME_LEN];
    char parent[MAX_PATH_LEN];
    inode_t parent_dir = -1;
    inode_t remove_dir = -1;
    int f = 0;
    for(int x = strlen(path); x > 0; x--) {
        if(path[x] == '/') {
            f = 1;
            strlcpy(remove, &path[x]+1, strlen(path) - x+1);
            strlcpy(parent, &path[0], x+1);
            parent_dir = name2inode(parent);
            remove_dir = name2inode(path);
            break;
        }
    }
    if(!f) {
        strlcpy(remove, path, strlen(path)+1);
        remove_dir = name2inode(remove);
        parent_dir = current_running->cwd;
    }
    // Not allowed to delete the self and parent entries
    if(strncmp(remove, ".", strlen(remove)) == 0 || strncmp(remove, "..", strlen(remove)) == 0) {
        return FSE_INVALIDNAME;
    }

    if(remove_dir < 0 || parent_dir < 0 || inodes[parent_dir].d_inode.type != INTYPE_DIR || inodes[remove_dir].d_inode.type != INTYPE_DIR) {
        return FSE_NOTEXIST;
    }

    remove_directory_entry(parent_dir, remove_dir);

    return FSE_OK;
}
/* Creates a harlink to a file
 * if the file is not found returns file not found
 * otherwise returns if the harlink creation was a success or not
 */
int fs_link(char *linkname, char *filename)
{
    inode_t id = name2inode(filename);
    if(id < 0 || inodes[id].d_inode.type == INTYPE_DIR) {
        return FSE_NOTEXIST;
    }
    if(current_running->cwd <= 0) {
        current_running->cwd = super.root_inode;
    }
    return create_directory_entry(current_running->cwd, id, linkname);
}
/* Removes a hardlink to the file
 * returns file not found if file does not exist otherwise FSE_OK
 * If the hardlink is the last one for the file the file also gets deleted
 */
int fs_unlink(char *linkname)
{
    if(current_running->cwd <= 0) {
        current_running->cwd = super.root_inode;
    }
    inode_t id = name2inode_f(current_running->cwd, linkname);
    if(id < 0) {
        return FSE_NOTEXIST;
    }
    remove_directory_entry(current_running->cwd, id);
    return FSE_OK;
}
/* Writes inode stats to the buffer
 *
 */
int fs_stat(int fd, char *buffer)
{
    if(current_running->filedes[fd].mode == MODE_UNUSED) {
        return FSE_INVALIDMODE;
    }
    int id = current_running->filedes[fd].idx;
    struct disk_inode *d = &inodes[id].d_inode;
    buffer[0] = d->type;
    buffer[1] = d->nlinks;
    bcopy((char*)&d->size, &buffer[2], sizeof(int));
    return FSE_OK;
}

/*
 * Helper functions for the system calls
 */

/*
 * get_free_entry:
 *
 * Search the given bitmap for the first zero bit.  If an entry is
 * found it is set to one and the entry number is returned.  Returns
 * -1 if all entrys in the bitmap are set.
 */
static int get_free_entry(unsigned char *bitmap)
{
    int i;

    /* Seach for a free entry */
    for (i = 0; i < BITMAP_ENTRIES / 8; i++) {
        if (bitmap[i] == 0xff)          /* All taken */
            continue;
        if ((bitmap[i] & 0x80) == 0) {  /* msb */
            bitmap[i] |= 0x80;
            return i * 8;
        } else if ((bitmap[i] & 0x40) == 0) {
            bitmap[i] |= 0x40;
            return i * 8 + 1;
        } else if ((bitmap[i] & 0x20) == 0) {
            bitmap[i] |= 0x20;
            return i * 8 + 2;
        } else if ((bitmap[i] & 0x10) == 0) {
            bitmap[i] |= 0x10;
            return i * 8 + 3;
        } else if ((bitmap[i] & 0x08) == 0) {
            bitmap[i] |= 0x08;
            return i * 8 + 4;
        } else if ((bitmap[i] & 0x04) == 0) {
            bitmap[i] |= 0x04;
            return i * 8 + 5;
        } else if ((bitmap[i] & 0x02) == 0) {
            bitmap[i] |= 0x02;
            return i * 8 + 6;
        } else if ((bitmap[i] & 0x01) == 0) { /* lsb */
            bitmap[i] |= 0x01;
            return i * 8 + 7;
        }
    }
    return -1;
}

/*
 * free_bitmap_entry:
 *
 * Free a bitmap entry, if the entry is not found -1 is returned, otherwise zero.
 * Note that this function does not check if the bitmap entry was used (freeing
 * an unused entry has no effect).
 */
static int free_bitmap_entry(int entry, unsigned char *bitmap)
{
    unsigned char *bme;

    if (entry >= BITMAP_ENTRIES)
        return -1;

    bme = &bitmap[entry / 8];

    switch (entry % 8) {
    case 0:
        *bme &= ~0x80;
        break;
    case 1:
        *bme &= ~0x40;
        break;
    case 2:
        *bme &= ~0x20;
        break;
    case 3:
        *bme &= ~0x10;
        break;
    case 4:
        *bme &= ~0x08;
        break;
    case 5:
        *bme &= ~0x04;
        break;
    case 6:
        *bme &= ~0x02;
        break;
    case 7:
        *bme &= ~0x01;
        break;
    }

    return 0;
}



/*
 * ino2blk:
 * Returns the filesystem block (block number relative to the super
 * block) corresponding to the inode number passed.
 */
static blknum_t ino2blk(inode_t ino)
{
    if(ino < 0 || ino > 512) {
        return -1;
    }
    // We round up a inode to take 32 bytes
    // That means we have space for 16 inodes in a block, block 512 bytes / 1 sector
    int space = BLOCK_SIZE / INODE_SIZE;
    return SUPER_BLOCK_START + BITMAP_BLOCKS + (ino / space) + 1; // superblock + 2 bitmaps + block belonging to inode
}

/*
 * idx2blk:
 * Returns the filesystem block (block number relative to the super
 * block) corresponding to the data block index passed.
 */
static blknum_t idx2blk(int index)
{
    if(index < 0 || index > FS_BLOCKS - INODE_BLOCKS - BITMAP_BLOCKS - 1) {
        return -1;
    }
    return SUPER_BLOCK_START + BITMAP_BLOCKS + INODE_BLOCKS + index;
}


/* Tries to find file in directory.
 *
 */
static inode_t name2inode_f(int dir, char *name) {
    struct mem_inode *inode = &inodes[dir];
    char buffer[inode->d_inode.size];
    db_read(dir, buffer, inode->d_inode.size, 0);
    for(int y = 0; y < inode->d_inode.size; y += sizeof(struct dirent)) {
        struct dirent* entry = &buffer[y];
        if(strncmp(entry->name, name, strlen(name)) == 0) {
            return entry->inode;
        }
    }
    return -1;
}

/* Recursively travels directories to find the file/directory
 *
 */
static inode_t name2inode_r(int dir, char *name)
{
    if(strlen(name) == 0) {
        return dir;
    }

    char path[MAX_PATH_LEN];
    struct mem_inode *inode = &inodes[dir];

    int len = strlen(name);
    for(int i = 0; i < len; i++) {
        if(name[i] == '/') {
            strlcpy(path, name, i+1);
            struct mem_inode *inode = &inodes[dir];
            char buffer[inode->d_inode.size];
            db_read(dir, buffer, inode->d_inode.size, 0);
            for(int y = 0; y < inode->d_inode.size; y += sizeof(struct dirent)) {
                struct dirent* entry = &buffer[y];
                if(strncmp(entry->name, path, strlen(path)) == 0) {
                    return name2inode_r(entry->inode, &name[i+1]);
                }
            }
            return -1;
        }
    }
    return name2inode_f(dir, name);
}

/*
 * name2inode:
 * Parses a file name and returns the corresponding inode number. If
 * the file cannot be found, -1 is returned.
 */
static inode_t name2inode(char *name)
{
    return name2inode_r(current_running->cwd, name);
}


