/*
 * Implementation of the mailbox.
 * Implementation notes: 
 *
 * The mailbox is protected with a lock to make sure that only 
 * one process is within the queue at any time. 
 *
 * It also uses condition variables to signal that more space or
 * more messages are available. 
 * In other words, this code can be seen as an example of implementing a
 * producer-consumer problem with a monitor and condition variables. 
 *
 * Note that this implementation only allows keys from 0 to 4 
 * (key >= 0 and key < MAX_Q). 
 *
 * The buffer is a circular array. 
*/

#include "common.h"
#include "thread.h"
#include "mbox.h"
#include "util.h"
#include "syslib.h"

mbox_t Q[MAX_MBOX];


/*
 * Returns the number of bytes available in the queue
 * Note: Mailboxes with count=0 messages should have head=tail, which
 * means that we return BUFFER_SIZE bytes.
 */
static int space_available(mbox_t * q)
{
  if ((q->tail == q->head) && (q->count != 0)) {
    /* Message in the queue, but no space  */
    return 0;
  }

  if (q->tail > q->head) {
    /* Head has wrapped around  */
    return q->tail - q->head;
  }
  /* Head has a higher index than tail  */
  return q->tail + BUFFER_SIZE - q->head;
}

/* Initialize mailbox system, called by kernel on startup  */
void mbox_init(void)
{
    // Set counters to 0 and initialize locks and conditions for
    // each mailbox
    for(int i = 0; i < MAX_MBOX; i++) {
        Q[i].used = 0;
        lock_init(&Q[i].l);
        condition_init(&Q[i].moreSpace);
        condition_init(&Q[i].moreData);
        Q[i].count = 0;
        Q[i].head = 0;
        Q[i].tail = 0;
    }
}

// Helper functions
void mbox_acquire(int key) {
    //ASSERT2(key >= 0 && key < MAX_MBOX, "No mailbox exists for given key");
    // Instead of halting the entire system we will just exit
    // the thread/process trying to open a non-existant mailbox
    if(key < 0 || key >= MAX_MBOX) {
        scrprintf(0,0,"Error: PID: %i", current_running->pid);
        scrprintf(1,0,"Attemted to access");
        scrprintf(2,0,"non-existant mailbox");
        exit();
    }

    lock_acquire(&Q[key].l);
}
// Should only be called internaly after acquire() so we know key should be valid.
void mbox_release(int key) {
    lock_release(&Q[key].l);
}

/*
 * Open a mailbox with the key 'key'. Returns a mailbox handle which
 * must be used to identify this mailbox in the following functions
 * (parameter q).
 */
int mbox_open(int key)
{
    mbox_acquire(key);
    Q[key].used++;
    mbox_release(key);
    return key;
}

/* Close the mailbox with handle q  */
//TODO: What did they mean reclaim data structure?
int mbox_close(int q)
{
    mbox_acquire(q);
    if(Q[q].used > 0) {
        Q[q].used--;
    }

    // Not sure what you ment reclaim data structure
    // So im just waking up everything waiting
    // And resetting all the locks and counters/pointers
    // So everything is basically unlocked and
    // all the space is "free" (can be written over)
    if(Q[q].used <= 0) {
        condition_broadcast(&Q[q].moreSpace);
        condition_broadcast(&Q[q].moreData);
        Q[q].used = 0;
        lock_init(&Q[q].l);
        condition_init(&Q[q].moreSpace);
        condition_init(&Q[q].moreData);
        Q[q].count = 0;
        Q[q].head = 0;
        Q[q].tail = 0;
    }

    mbox_release(q);
    return q;
}

/*
 * Get number of messages (count) and number of bytes available in the
 * mailbox buffer (space). Note that the buffer is also used for
 * storing the message headers, which means that a message will take
 * MSG_T_HEADER + m->size bytes in the buffer. (MSG_T_HEADER =
 * sizeof(msg_t header))
 */
int mbox_stat(int q, int *count, int *space)
{
    mbox_acquire(q);
    *count = Q[q].count;
    *space = space_available(&Q[q]);
    mbox_release(q);
    return 1;
}

/* Fetch a message from queue 'q' and store it in 'm'  */
int mbox_recv(int q, msg_t * m)
{
    mbox_acquire(q);

    // If theres no messages we wait for one to arrive
    // Always use while loop, condition might be woken up by mistake.
    // Better to recheck and make sure before moving on.
    while (Q[q].count <= 0) {
        // condition_wait will release the key we locked in mbox_acquire
        condition_wait(&Q[q].l, &Q[q].moreData);
    }

    // We need to read msg first to get the size
    int start = Q[q].tail; // Not nessesary, just make it easier to read
    int size = sizeof(m);
    for(int i = 0; i < size; i++) {
        ((char *)m)[i] = ((char *)Q[q].buffer)[(start + i) % BUFFER_SIZE];
    }
    // Read in the entire message
    size = sizeof(m) + m->size;
    for(int i = 0; i < size; i++) {
        ((char *)m)[i] = ((char *)Q[q].buffer)[(start + i) % BUFFER_SIZE];
    }

    // Update tail pointer
    Q[q].tail = (start + size) % BUFFER_SIZE;
    Q[q].count--;
    // Let people waiting on send know theres more free space
    condition_broadcast(&Q[q].moreSpace);
    mbox_release(q);
    return 1;
}

/* Insert 'm' into the mailbox 'q'  */
int mbox_send(int q, msg_t * m)
{
    mbox_acquire(q);
    // Make sure we have enought free space for text and the header
    while (((int)sizeof(m) + m->size) > space_available(&Q[q])) {
        condition_wait(&Q[q].l, &Q[q].moreSpace);
    }
    // Write msg to mailbox buffer
    int start = Q[q].head;
    int size = sizeof(m) + m->size;
    for(int i = 0; i < size; i++) {
        ((char *)Q[q].buffer)[(start + i) % BUFFER_SIZE] = ((char *)m)[i];
    }
    // Update head pointer
    Q[q].head = (start + size) % BUFFER_SIZE;
    Q[q].count++;
    // Signal that messages are available
    condition_broadcast(&Q[q].moreData);
    mbox_release(q);
    return 1;
}

