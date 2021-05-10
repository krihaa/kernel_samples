#  entry.S
#  This file contains the code for kernel_entry, as well as
#  scheduler_entry, which saves the context of the current process
#  before calling the scheduler. The file is pre-processed before it
#  is assembled, allowing (among other things) the use of macros
#  and C-style comments.

.data
.align 4
.text
.code32
.align 4

# Make symbols visible for ld
.globl kernel_entry
.globl scheduler_entry
.globl start_process
.globl start_thread


# void kernel_entry(int fn)
#
# SYSCALL(i) in syslib.c calls this function
# fn = system call number (defined in kernel.h)
#
ke_scratch:
  .long	0
ke_arg:
  .long	0

# Inside kernel_entry we save registers, flags then the stack pointer.
# then we switch to user stack before calling kernel_entry_helper
# When we return we restore what we saved
kernel_entry:
  pushl %edx
  pushl %eax
  call start_timer
  popl %eax
  popl %edx

  # Save eax
  movl	%eax, (ke_scratch)

  # Save argument
  movl	4(%esp), %eax
  movl	%eax, (ke_arg)

  # Now it's up to you!

  movl (ke_scratch), %eax

  # put registers and flags on stack
  pushal
  pushfl

  movl (current_running), %eax 
  movl %esp, 8(%eax) # save stack pointer

  movl 12(%eax), %esp # switch to user stack

  pushl ke_arg
  call kernel_entry_helper # call entry_helper with argument
  popl ke_arg

  # restore process
  movl (current_running), %eax
  movl %esp, 12(%eax)
  movl 8(%eax), %esp # restore process stack

  # restore registers and flags from the stack
  popfl
  popal


  ret

loop:
  jmp		loop

# This function gets called to enter the scheduler,
# saving registers before doing so.

# In here we save the general registers, flags and bits used for floating point operations
# in the user stack + save stack pointer address, before calling the scheduler
# When we return we restore what we saved
scheduler_entry:
  # Fill in the blanks!
  pushl %edx
  pushl %eax
  call start_timer
  popl %eax
  popl %edx

  # save registers and flags on stack
  pushal
  pushfl


  subl $108, %esp # fsave uses 108 bytes in protected(32bit) mode
  #fxsave
  fwait
  fsave (%esp) # save fpu,mmx,sse

  movl (current_running), %eax
  movl %esp, 12(%eax) # save stack pointer

  call scheduler

  # restore process
  movl (current_running), %eax
  movl 12(%eax), %esp # restore process stack

  frstor (%esp) # restore fpu,mmx,sse
  addl $108, %esp

  # restore registers and flags
  popfl
  popal
  
  pushl %edx
  pushl %eax
  call end_timer
  popl %eax
  popl %edx

  ret

# process starts at kernel stack
start_process:
  movl (current_running),%eax
  movl 8(%eax), %esp
  call 4(%eax)
# threads start at user stack, since they dont use the kernel stack
start_thread:
  movl (current_running), %eax
  movl 12(%eax), %esp
  call 4(%eax)

