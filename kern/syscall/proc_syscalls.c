#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include "opt-A2.h"

/* this implementation of sys__exit does not do anything with the exit code */
/* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {
	struct addrspace *as;
	struct proc *p = curproc;
	/* for now, just include this to keep the compiler from complaining about
	   an unused variable */

	DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);
	DEBUG(DB_SYSCALL,"Syscall: pid(%d)\n", p->pid);

	KASSERT(curproc->p_addrspace != NULL);
	as_deactivate();
	/*
	 * clear p_addrspace before calling as_destroy. Otherwise if
	 * as_destroy sleeps (which is quite possible) when we
	 * come back we'll be calling as_activate on a
	 * half-destroyed address space. This tends to be
	 * messily fatal.
	 */
	as = curproc_setas(NULL);
	as_destroy(as);

#if OPT_A2

	bool destroy = false;
	lock_acquire(curproc->info_lock);	

	// delete children that have already exited
	unsigned i = 0;
	while(i < array_num(curproc->children)){
		if (((struct proc *)(array_get(curproc->children, i)))->exited){
			proc_destroy((struct proc *)(array_get(curproc->children, i)));
			array_remove(curproc->children, i);
		}else{
			((struct proc *)(array_get(curproc->children, i)))->parent = NULL;
			i++;
		}
	}

	if (curproc->parent == NULL){
		destroy = true;
	}else{
		curproc->exited = true;
		curproc->exitstatus = _MKWAIT_EXIT(exitcode);
		cv_signal(curproc->parentSignal, curproc->info_lock); // signal to parent that child has exited
	}

	lock_release(curproc->info_lock);	
	proc_remthread(curthread);
	if (destroy) proc_destroy(p);
#else
	(void)exitcode;
	/* detach this thread from its process */
	/* note: curproc cannot be used after this call */
	proc_remthread(curthread);

	/* if this is the last user process in the system, proc_destroy()
	   will wake up the kernel menu thread */
	proc_destroy(p);
#endif
	thread_exit();
	/* thread_exit() does not return, so we should never get here */
	panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
	int
sys_getpid(pid_t *retval)
{
#if OPT_A2
	struct proc *current = curproc;
	lock_acquire(current->info_lock);
	*retval = current->pid;
	lock_release(current->info_lock);
#else
	/* for now, this is just a stub that always returns a PID of 1 */
	/* you need to fix this to make it work properly */
	*retval = 1;
#endif
	return(0);
}

/* stub handler for waitpid() system call                */

	int
sys_waitpid(pid_t pid,
		userptr_t status,
		int options,
		pid_t *retval)
{
	int exitstatus;
	int result;

	/* this is just a stub implementation that always reports an
	   exit status of 0, regardless of the actual exit status of
	   the specified process.
	   In fact, this will return 0 even if the specified process
	   is still running, and even if it never existed in the first place.
	   Fix this!
	   */

	if (options != 0) {
		return(EINVAL);
	}
#if OPT_A2
	exitstatus = ECHILD;
	lock_acquire(curproc->info_lock);
	unsigned n = array_num(curproc->children);
	for (unsigned i = 0; i < n; i++){
		struct proc *child = (struct proc *)(array_get(curproc->children, i));
		lock_acquire(child->info_lock);
		if(child->pid == pid){
			if (!child->exited){
				cv_wait(child->parentSignal, child->info_lock);
			}
			exitstatus = child->exitstatus;

			// delete child
			lock_release(child->info_lock);
			proc_destroy(child);
			array_remove(curproc->children, i);
			break;
		}
		lock_release(child->info_lock);
	}
	lock_release(curproc->info_lock);
#else
	/* for now, just pretend the exitstatus is 0 */
	exitstatus = 0;
#endif
	result = copyout((void *)&exitstatus,status,sizeof(int));
	if (result) {
		return(result);
	}
	*retval = pid;
	return(0);
}

#if OPT_A2
pid_t sys_fork(struct trapframe *tf, pid_t *retval){
	KASSERT(tf != NULL);
	KASSERT(retval != NULL);
	KASSERT(curproc != NULL);

	struct proc *child = proc_create_runprogram(curproc->p_name);
	if(child == NULL){
		return (ENOMEM);
	}

	struct addrspace *childAS = NULL;
	struct addrspace *parentAS = curproc_getas();

	KASSERT(parentAS != NULL);
	int exitstatus = as_copy(parentAS, &childAS);
	if (exitstatus){
		return (exitstatus);
	}	

	struct cv *wait = cv_create(curproc->p_name);

	spinlock_acquire(&child->p_lock);
	child->p_addrspace = childAS;
	spinlock_release(&child->p_lock);

	lock_acquire(child->info_lock);
	child->parent = curproc;
	child->parentSignal = wait;
	lock_release(child->info_lock);

	lock_acquire(curproc->info_lock);
	array_add(curproc->children, child, NULL);
	lock_release(curproc->info_lock);

	*retval = child->pid;

	exitstatus = thread_fork(curthread->t_name, child, enter_forked_process, tf, 0);
	if (!exitstatus){
		return (exitstatus);
	}
	return (0);
}
#endif


#if OPT_A2
int sys_execv(userptr_t progname, userptr_t args){
  struct addrspace *as;
        struct vnode *v;
        vaddr_t entrypoint, stackptr;
        int result;

	size_t len = 0;
    
	// copy program path
	char *kernelProgram = (char *)(kmalloc(128 * sizeof(char)));
	copyinstr(progname, kernelProgram, 128, &len);
	
	// kprintf("program name: ");
	// kprintf(kernelProgram);
       	// kprintf("\n");

	
	// copy args and get argc
	int argc = 0;
	// char** kernelArgs = (char **)(kmalloc(sizeof(char *)));
	//char **index = (char **)(args);
	
	char **argPtr = (char **)(kmalloc(sizeof(char *)));
	copyin(args, argPtr, 4);
	while(*argPtr != NULL){
		argc++;
		copyin((const_userptr_t)(args + argc * sizeof(char**)), argPtr, 4);
	}        

	char **kernelArgs = (char **)(kmalloc((argc + 1) * sizeof(char *)));
	for (int i = 0; i < argc; i++){
		size_t len = 0;
		copyin((const_userptr_t)(args + i * sizeof(char**)), argPtr, 4);
		kernelArgs[i] = (char *)(kmalloc(128 * sizeof(char)));
		copyinstr((const_userptr_t)(*argPtr), (kernelArgs[i]), 128, &len); 
	}
	 kernelArgs[argc] = NULL;


  
	/* Open the file. */
        result = vfs_open(kernelProgram, O_RDONLY, 0, &v);
        if (result) {
                return result;
        }

        struct addrspace *oldAS = curproc_getas();

	/* Create a new address space. */
        as = as_create();
        if (as ==NULL) {
                vfs_close(v);
                return ENOMEM;
        }

        /* Switch to it and activate it. */
        curproc_setas(as);
        as_activate();

        /* Load the executable. */
        result = load_elf(v, &entrypoint);
        if (result) {
                /* p_addrspace will go away when curproc is destroyed */
                vfs_close(v);
                return result;
        }

        /* Done with the file now. */
        vfs_close(v);

       // /* Define the user stack in the address space */
       	result = as_define_args(as, kernelArgs, argc, &stackptr);
        if (result) {
                /* p_addrspace will go away when curproc is destroyed */
                return result;
        }

	kfree(argPtr);
	kfree(kernelArgs);
	as_destroy(oldAS);
        
	/* Warp to user mode. */
        enter_new_process(argc /*argc*/, (userptr_t)(as->argv) /*userspace addr of argv*/,
                          stackptr, entrypoint);

        /* enter_new_process does not return. */
        panic("enter_new_process returned\n");
        return EINVAL;
}

#endif
