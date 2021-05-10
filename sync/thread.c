/*
 * Implementation of locks and condition variables
 */

#include "common.h"
#include "util.h"
#include "thread.h"
#include "scheduler.h"
#include "interrupt.h"

void lock_init(lock_t * l)
{
  /*
   * no need for critical section, it is callers responsibility to
   * make sure that locks are initialized only once
   */
  l->status = UNLOCKED;
  l->waiting = NULL;
}

/* Acquire lock whitout critical section (called whitin critical section) */
static void lock_acquire_helper(lock_t * l)
{
  if(l->status == UNLOCKED) {
    l->status = LOCKED;
  }
  else { // Somebody owns the lock, get put in queue
    block(&l->waiting);
  }
}

void lock_acquire(lock_t * l)
{
  enter_critical();
  lock_acquire_helper(l);
  leave_critical();
}

void lock_release(lock_t * l)
{
  enter_critical();
  if(l->waiting == NULL) {
    l->status = UNLOCKED;
  }
  else {
    unblock(&l->waiting);
  }
  leave_critical();
}

/* condition functions */

void condition_init(condition_t * c)
{
  c->waiting = NULL;
}

/*
 * unlock m and block the thread (enqued on c), when unblocked acquire
 * lock m
 */
void condition_wait(lock_t * m, condition_t * c)
{
  lock_release(m);
  enter_critical();
  block(&c->waiting);
  lock_acquire_helper(m);
  leave_critical();
}

/* unblock first thread enqued on c */
void condition_signal(condition_t * c)
{
  enter_critical();
  if(c->waiting != NULL) {
    unblock(&c->waiting);
  }
  leave_critical();
}

/* unblock all threads enqued on c */
void condition_broadcast(condition_t * c)
{
  enter_critical();
  while(c->waiting != NULL) { //loop trought all threads waiting
    unblock(&c->waiting);
  }
  leave_critical();
}

/* Semaphore functions. */
void semaphore_init(semaphore_t * s, int value)
{
  s->waiting = NULL;
  s->counter = value;
}

void semaphore_up(semaphore_t * s)
{
  enter_critical();
  s->counter++;
  if(s->counter >= 0) {
    if(s->waiting != NULL) {
      unblock(&s->waiting);
    }
  }
  leave_critical();
}

void semaphore_down(semaphore_t * s)
{
  enter_critical();
  s->counter--;
  if(s->counter < 0) {
    block(&s->waiting);
  }
  leave_critical();
}

/*
 * Barrier functions
 * Note that to test these functions you must set NUM_THREADS
 * (kernel.h) to 9 and uncomment the start_addr lines in kernel.c.
 */

/* n = number of threads that waits at the barrier */
void barrier_init(barrier_t * b, int n)
{
  b->counter = 0;
  b->reach = n;
  b->waiting = NULL;
}

/* Wait at barrier until all n threads reach it */
void barrier_wait(barrier_t * b)
{
  enter_critical();
  b->counter++;
  if(b->counter == b->reach){
    while(b->waiting != NULL) { //loop trought all threads waiting
      unblock(&b->waiting);
    }
    b->counter = 0;
  }
  else {
    block(&b->waiting);
  }
  leave_critical();
}
