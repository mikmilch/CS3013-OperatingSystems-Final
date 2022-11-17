#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "gompeimalloc.h"

node_t *listHead = NULL;
int statusno;
int adjustedSize;
void* _arena_start;
int initialized = 0;

//Return the # of bytes mapped for the arena, if the mapping was successful
//Error: Return one of error codes in gompeimalloc.h
//Ex: call init(1) returns arena of 4096 (page size on most modern machines)
extern int init(size_t size){
    printf("Initializing arena: \n");

    printf("...requested size %ld bytes\n", size);

    //If input size is negative, return ERR_BAD_ARGUMENTS
    if((size+1) <= 0) return ERR_BAD_ARGUMENTS;

    //use the libc function getpagesize() to determine page size of current machine
    int pagesize = -1;
    pagesize = getpagesize();
    printf("...pagesize is %i bytes\n", pagesize);


    //"size" = min size of arena; BUT may have to inc. arena's size so arena is multiple of the page size
    printf("...adjusting size with page boundaries\n");

    //Size of of arena is determined by input parameter "size" and the page size.
    adjustedSize = size;
    //check if requested size is mutltiple of page size
    int multiple = size % pagesize;
    int min = pagesize; //minimum pagesize
    //if not a multiple
    if(multiple != 0){
        while(size > min){
            min = min + pagesize;
        }
        adjustedSize = min;
    }

    printf("...adjusted size is %i bytes\n", adjustedSize);

    //Mapping arena
    int fd = open("/dev/zero", O_RDWR);
    _arena_start = mmap(NULL, adjustedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    printf("...mapping arena with mmap()\n");

    //arena start and end size
    int startHex = *((int*)_arena_start);
    int endHex = startHex + adjustedSize;
    printf("...arena starts at %i \n", startHex);
    printf("...arena ends at %i \n", endHex);

    //initialize header / chunk list
    //Create head of linked list
    //starts at slot 0 of arena
    //mmap returns start of arena ***
    //list head is particular structure, node_t
    //so change values in __node_t
    //node_t *listHead;
    listHead = (node_t *) _arena_start;
    listHead->is_free = 1;
    listHead->fwd = NULL;
    listHead->bwd = NULL;
    listHead->size = adjustedSize - sizeof(node_t);

    printf("...initializing header for initial free chunk \n");

    //give header size
    //
    int headerSize = listHead->size;
    // think header size was wrong based on forum post
    printf("...header size is %lu bytes \n", sizeof(node_t));

    statusno = 0;
    
    initialized = 1;

    return listHead->size + sizeof(node_t);
}

extern int destroy(){
    printf("Destroying Arena: \n");
    printf("...unmapping arena with munmap() \n");

    if(initialized == 0) return ERR_UNINITIALIZED;

    //use munmap() system call to return the arena's memory to the OS
    int success = munmap(_arena_start, adjustedSize);
    //reset any variables used to track internal state
    initialized = 0;

    return success;
}

extern void* walloc(size_t size){
    void* buf;
    printf("Allocating memory: \n");

    if(initialized == 0) {
        printf("...arena is uninitialized\n");
        statusno = ERR_UNINITIALIZED;
        return NULL;
    }

    printf("...looking for free chunk of >= %lu bytes\n", size);

    node_t* current = listHead;

    while(current->is_free == 0 || (current->size) < size) {
        if(current->fwd != NULL) {
            current = current->fwd;
        } else {
            statusno = ERR_OUT_OF_MEMORY;
            return NULL;
        }
    }

    printf("...found free chunk of %lu bytes with header at %p\n", current->size, current);
    printf("...free chunk->fwd currently points %p\n", current->fwd);
    printf("...free chunk->bwd currently points %p\n", current->bwd);

    node_t* fwd1 = current->fwd;
    size_t size1 = current->size;

    printf("...checking if splitting is required\n");
    
    //chunkSize - requested Size = size left
    //chunkLeft > size of node_t

    if((int) (current->size - size) > (int) sizeof(node_t)) {
        printf("...splitting is required\n");
	//make new unused chunk
	node_t* newFree = ((void*)current) + sizeof(node_t) + size;
    	newFree->is_free = 1;
    	newFree->fwd = current->fwd;
   	newFree->bwd = current;
    	newFree->size = current->size - size - sizeof(node_t);
	
	//update current
	current->fwd = newFree;
	current->size = size;

    } else {
        printf("...splitting not required\n") ;
    }

    printf("...updating chunk header at %p\n", current);
    current->is_free = 0;

    printf("...being careful with my pointer arithmetic and void pointer casting\n");
    printf("...allocation starts at %p\n", current+1);

    buf = current+1;
    return buf;

};
extern void wfree(void *ptr){
    printf("Freeing allocated memory:\n");
    printf("...supplied pointer %p\n", ptr);
    printf("...being careful with my pointer arithmetic and void pointer casting\n");

    node_t* current = ((node_t*)ptr) - 1;

    printf("...accessing chunk header at %p\n", current);
    printf("...chunk size of %lu\n", current->size);

    current->is_free = 1;

    printf("...checking if coalescing is needed\n");

    if(current->fwd != NULL && current->bwd != NULL && current->fwd->is_free == 1 && current->bwd->is_free == 1) {
        printf("...coalescing needed for both fwd and bwd\n");

        current->bwd->fwd = current->fwd->fwd;
        node_t* new = current->fwd->fwd;

        if(new != NULL) {
            new->bwd = current->bwd;
        }

        current->bwd->size = current->bwd->size + current->size + current->fwd->size + 2 * sizeof(node_t);
    } else if(current->bwd != NULL && current->bwd->is_free == 1) {
        printf("...coalescing needed for bwd\n");

        current->bwd->fwd = current->fwd;
        node_t* new = current->fwd;

        if(new != NULL) {
            new->bwd = current->bwd;
        }

        current->bwd->size = current->bwd->size + current->size + sizeof(node_t);
    } else if(current->fwd != NULL && current->fwd->is_free == 1) {
        printf("...coalescing needed for fwd\n");

        current->fwd = current->fwd->fwd;
        node_t* new = current->fwd->fwd;

        if(new != NULL) {
            new->bwd = current;
        }

        current->size = current->size + current->fwd->size + sizeof(node_t);
    } else {
        printf("...coalescing not needed\n");
    }


};

/*
void main(){
	printf("Hello world\n");
	init(4);
	destroy();
}
*/

