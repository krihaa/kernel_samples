#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_FILE "image"
#define ARGS "[--extended] [--vm] <bootblock> <executable-file> ..."

#define SECTOR_SIZE 512       /* USB sector size in bytes */
#define OS_SIZE_LOC 2         /* OS position within bootblock */  
#define BOOT_MEM_LOC 0x7c00   /* bootblock memory location */
#define OS_MEM_LOC 0x8000     /* kernel memory location */

#define bool int
#define true 1
#define false 0

// Structure to hold a data segment from elf parsing
// size of the data and a pointer to it
struct Segment{
  int size;
  char* data;
};

// For the --extended argument
bool extended = false;

struct Segment* parse_file(char* filename, int* segments_count, int* memory_size);

/*
 * Frees all the data segments from a array
 *   Args:
 *      Segment* d : pointer to beginning of array
 *      int c : number of elements in array
 */
void free_segments(struct Segment* d, int c) {
  for (int i = 0; i < c; i++) {
    free(d[i].data);
  }
  free(d);
}

/*
 * Writes all the data segments in a array to a file
 * Args:
 *    Segment* d : pointer to start of array
 *    int c : length of array
 *    FILE *f : pointer to the file
 */

void write_segments(struct Segment*d, int c, FILE *f){
  for (int i = 0; i < c; i++) {
    if(fwrite(d[i].data, d[i].size, 1, f) != 1) {
      printf("Could not write successfully to image\n");
    }
  }
}

/*
 * Main function, takes a minimum of 2 parameters up to 3
 * Reads inn a bootloader and kernel file and creates a bootable image
 * Parameters:
 *         [--extended] : write out additional debugg info
 *         bootblock    : name of bootblock file
 *         kernel       : name of kernel file
 */

int main(int argc, char **argv)
{
  if(argc < 3) {
    printf("Wrong arguments, run the program with ./createimage [--extended] bootblock kernel\n");
    return 0;
  }

  int boot_segments = 0;
  int boot_size = 0;
  char* boot_name;
  int kernel_segments = 0;
  int kernel_size = 0;
  char* kernel_name;

  // depending on number of arguments supplied
  if(strcmp(argv[1],"--extended") == 0) {
    if(argc < 4) {
      printf("Wrong arguments, run the program with ./createimage [--extended] bootblock kernel\n");
      return 0;
    }
    extended = true;
    boot_name = argv[2];
    kernel_name = argv[3];
  }else {
    boot_name = argv[1];
    kernel_name = argv[2];
  }

  // read in files
  printf("0x%x  %s\n",BOOT_MEM_LOC, boot_name);
  struct Segment* boot_data = parse_file(boot_name, &boot_segments, &boot_size);
  printf("0x%x  %s\n",OS_MEM_LOC, kernel_name);
  struct Segment* kernel_data = parse_file(kernel_name, &kernel_segments, &kernel_size);


  // if the files seems correct, write them to image
  if(boot_segments> 0 && kernel_segments > 0 && boot_size == 512 && kernel_size > 0){
    FILE *f = fopen("image","wb");
    if(f == NULL){
      printf("Failed to create image file\n");
    }else{
      write_segments(boot_data, boot_segments, f);
      write_segments(kernel_data, kernel_segments, f);
      // if the kernel doesnt fit a factor of 512, add 0 bytes to make it
      if(kernel_size % SECTOR_SIZE != 0) {
        int rem = SECTOR_SIZE - (kernel_size % SECTOR_SIZE);
        if(extended) {
          printf("padding os with: %d bytes\n", rem);
        }
        char filler[512] = {0};
        fwrite(filler,rem,1,f);
      }
      int os_size = (kernel_size / SECTOR_SIZE) + (kernel_size % SECTOR_SIZE != 0);
      if(extended) {
        printf("os_size: %d\n", os_size);
      }

      // Write the size of the kernel in the bootloader
      fseek(f, OS_SIZE_LOC, SEEK_SET);
      fwrite(&os_size, sizeof(os_size), 1, f);
      fclose(f);

    }

  }else {
    printf("Bootblock or Kernel file is incorrect\n");
  }

  if(boot_segments > 0){
    free_segments(boot_data, boot_segments);
  }
  if(kernel_segments > 0) {
    free_segments(kernel_data, kernel_segments);
  }
  return 0;
}


/*
  ELF FILE LAYOUT
  [ELF EHDR HEADER]
  [ELF PHDR HEADER]
  [DATA SEGMENT]
  [ELF PHDR HEADER]
  [DATA SEGMENT]
  etc..
*/


/*
 * Opens and read in a binary file and parse it according to the elf headers in the file
 * Args:
 *    char* filename : name of the file to open
 *    int* segments_count : sets the value of this int to the number of segments found in the filename
 *    int* memory_size : sets the value of this to the memory size of all the segments found in the file
 * Returns:
 *    Segment* : A pointer to a array of struct Segment, this array needs to be free'd manually
 */

struct Segment* parse_file(char* filename, int* segments_count, int* memory_size) {

  FILE *f = fopen(filename, "rb");
  if (!f) {
    printf("Could not find/open file %s\n",filename);
    return NULL;
  }
  Elf32_Ehdr header; // Elf header for 32bit programs, always at start of file
  if(fread(&header, sizeof(header), 1, f) != 1) {
    printf("Could not read elf header\n");
    fclose(f);
    return NULL;
  }

  int p_memsz = 0; // Total memory size of all segments combined

  struct Segment* segments = calloc(header.e_phnum,sizeof(struct Segment));

  // e_phnum is the number of program headers (PHDR) in the file
  for(int i = 0; i < header.e_phnum; i++) {
    Elf32_Phdr program_header;
    if(fread(&program_header, sizeof(program_header),1,f) != 1) {
      printf("Error reading program header\n");
      fclose(f);
      free_segments(segments, i);
      return NULL;
    }
    // Write additional debug info
    if(extended) {
      printf("%10s %d \n","Segment:",i);
      printf("%20s %d","memsz:", program_header.p_memsz);
      printf("%10s %d\n","filesz:", program_header.p_filesz);
      printf("%20s %d","offset:", program_header.p_offset);
      printf("%10s %d\n","vaddr:", program_header.p_vaddr);
    }
    p_memsz += program_header.p_memsz;
    segments[i].size = program_header.p_memsz;
    segments[i].data = calloc(program_header.p_memsz, 1);
    fread(segments[i].data, program_header.p_memsz, 1, f);
  }
  fclose(f);
  *segments_count = header.e_phnum;
  *memory_size = p_memsz;
  return segments;
}
