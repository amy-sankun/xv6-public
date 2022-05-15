#include "param.h"

// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
enum thrdstate { UNUSED_T, EMBRYO_T, SLEEPING_T, RUNNABLE_T, RUNNING_T, ZOMBIE_T };

typedef struct _thrd thrd;


// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  int killed;                  // If non-zero, have been killed
  thrd* threads;
  void *chan;                  // have to remove
};

struct _thrd {
  char *kstack;                // Bottom of kernel stack for this process
  enum thrdstate state;        // Thread state
  thread_t thid;               // Thread ID
  void *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  struct file *ofile[NOFILE];  // Open files
  thrd* next;
  void * ret_val;
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

// -------------------------------------------------------------------------
void myyield(void);

// custom proc structure for using mlfq
struct mproc {
  struct proc* p;
  int priority;
  struct mproc* prev;
  struct mproc* next;
  int ticks_a;                   // ticks for using mlfq time allotment
  int ticks_q;                   // ticks for using mlfq time quantum
};

#define MLFQ_SIZE 3  // number of mlfq level

typedef struct MLFQ{
    struct mproc procs[NPROC];                // mproc container 
    struct mproc* front[MLFQ_SIZE];           // q front 
    struct mproc* rear[MLFQ_SIZE];            //  q rear
    int ticks;                                // if ticks > 100 => boosting
}Mlfq;

typedef struct _Pass{
    struct proc* p;
    int stride;
    int ticket;
    int pass_value;
    int portion; // cpu share
}Pass;

typedef struct _Stride{
    int t_total; // number of tickets total
    int p_total; // number of pass total
    int num;
    Pass procs[NPROC]; // processes
    Pass mlfq; // mlfq
    int tick;
} Stride;


void initMLFQ();
void putMLFQ(struct proc* p);
struct mproc* changeMLFQ(struct mproc* mp, int priority);
struct mproc* getMLFQ();
void boostMLFQ();
struct proc* deleteMLFQ(int pid);
void printMLFQ(void);

int allocate_stride(int portion);
struct proc* get_stride();
void init_stride();
void delete_stride(int pid);
int find_min_stride();
void reduce_all_pass(int value);

void print_scheduler();
Mlfq mlfq;
Stride stride;


thrd* allocthrd(void);
int thread_create(thread_t * thread, void * (*start_routine)(void *), void *arg);
void thread_exit(void *retval);
int thread_join(thread_t thread, void **retval);
void thrd_init(thrd* t);
void init_thread_pool();

