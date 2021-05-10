/*  scheduler.c
  Best viewed with tabs set to 4 spaces.
*/
#include "common.h"
#include "kernel.h"
#include "scheduler.h"
#include "util.h"


/* Call scheduler to run the 'next' process */
void yield(void) {
    scheduler_entry();
}


/* The scheduler picks the next job to run, and removes blocked and exited
 * processes from the ready queue, before it calls dispatch to start the
 * picked process.
 */
void scheduler(void) {
    if(current_running->state == STATUS_BLOCKED || current_running->state == STATUS_EXITED) {
        if(current_running->state == STATUS_EXITED) {
            if(current_running->next == current_running) { // Removing last one
                //clear_screen(0,0,80,25);
                scrprintf(0,0, "All processes have exited");
                while(1); // nothing left to run, loop forever
            }
        }
        //Remove it from ready list
        current_running->previous->next = current_running->next;
        current_running->next->previous = current_running->previous;
        pcb_t* tmp = current_running;
        current_running = current_running->next;
        //Clear next and previous pointers, as they are used in blocked queue
        tmp->next = 0;
        tmp->previous = 0;
    }
    else {
        current_running = current_running->next;
    }
    dispatch();
}

/* dispatch() does not restore gpr's it just pops down the kernel_stack,
 * and returns to whatever called scheduler (which happens to be
 * scheduler_entry, in entry.S).
 */
void dispatch(void) {
    if(current_running->state == STATUS_FIRST_TIME) {
        current_running->state = STATUS_READY;
        start_process();
    }else if(current_running->state == STATUS_FIRST_TIME_THREAD) {
        current_running->state = STATUS_READY;
        start_thread();
    }
}

/* Remove the current_running process from the linked list so it
 * will not be scheduled in the future
 */
void exit(void) {
    current_running->state = STATUS_EXITED;
    scheduler_entry();
}


/* 'q' is a pointer to the waiting list where current_running should be * inserted */
void block(pcb_t **q) {
    current_running->state = STATUS_BLOCKED;
    if(*q == 0) { // No queue, place at front
        *q = current_running;
    }
    else { // Insert at end of queue
        pcb_t* tmp = *q;
        while(tmp->next != 0) {
            tmp = tmp->next;
        }
        tmp->next = current_running;
    }
    scheduler_entry();
}

/* Must be called within a critical section. 
 * Unblocks the first process in the waiting queue (q), (*q) points to the 
 * last process.
 */
void unblock(pcb_t **q) {
    pcb_t* tmp = *q;
    *q = tmp->next;
    // take it out of waiting queue
    //Place it behind current running in the queue
    tmp->state = STATUS_READY;
    tmp->previous = current_running->previous;
    tmp->next = current_running;
    current_running->previous->next = tmp;
    current_running->previous = tmp;
}


uint64_t timer = 0;
int type = 0;
int started = 0;
int nrOfSwitches = 0;
void start_timer(){
    if(started == 0) { // Dont reset timer if we are already timing
        type = current_running->type;
        timer = get_timer();
        started = 1;
    }
}
void end_timer(){
    if(started == 1) { // Make sure we are timing someting
        uint64_t duration = get_timer() - timer; // get duration as early as possible
        scrprintf(17,0,"                                                                ");
        scrprintf(17,50,"Count: %d.", ++nrOfSwitches);

        if(type == 0) { // Check if we are starting from process or thread
            if(current_running->type == 1) {
                scrprintf(17,0,"Contex-Switch Time Process->Thread: %d ticks."
                          , duration);
            }else {
                scrprintf(17,0,"Contex-Switch Time Process->Process: %d ticks."
                          , duration);
            }
        }else{
            if(current_running->type == 1) {
                scrprintf(17,0,"Contex-Switch Time Thread->Thread: %d ticks."
                          , duration);
            }else {
                scrprintf(17,0,"Contex-Switch Time Thread->Process: %d ticks."
                          , duration);
            }
        }
        started = 0;
    }
}
