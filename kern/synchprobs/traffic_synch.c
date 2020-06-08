#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <queue.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
Direction volatile currentOriginGo;
struct cv *NGo, *SGo, *EGo, *WGo;
struct queue *vehicleQueue;
struct lock *intersectionLock;
int volatile queueSize;
/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
	void
intersection_sync_init(void)
{
	intersectionLock = lock_create("intersectionLock");
	if (intersectionLock == NULL){
		panic("could not create intersection lock");
	}

	NGo = cv_create("NGo"); 
	if (NGo == NULL){
		panic("could not create NGo cv");
	}

	SGo = cv_create("SGo");
	if (SGo == NULL){
		panic("could not create SGo cv");
	}

	EGo = cv_create("EGo");
	if (EGo == NULL){
		panic("could not create EGo cv");
	}

	WGo = cv_create("WGo");
	if (WGo == NULL){
		panic("could not create WGo cv");
	}

	vehicleQueue = q_create(100);
	if (vehicleQueue == NULL){
		panic("could not create vehicle queue");
	}
	queueSize = 100;
	return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
	void
intersection_sync_cleanup(void)
{
	KASSERT(intersectionLock != NULL);
	KASSERT(vehicleQueue != NULL);
	KASSERT(NGo != NULL);
	KASSERT(SGo != NULL);
	KASSERT(EGo != NULL);
	KASSERT(WGo != NULL);

	q_destroy(vehicleQueue);
	lock_destroy(intersectionLock);
	cv_destroy(NGo);
	cv_destroy(SGo);
	cv_destroy(EGo);
	cv_destroy(WGo);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

	void
intersection_before_entry(Direction origin, Direction destination) 
{
	(void)destination; /* avoid compiler complaint about unused parameter */
	KASSERT(vehicleQueue != NULL);
	KASSERT(intersectionLock != NULL);
	KASSERT(NGo != NULL);
	KASSERT(SGo != NULL);
	KASSERT(EGo != NULL);
	KASSERT(WGo != NULL);

	lock_acquire(intersectionLock);
	if (q_empty(vehicleQueue)){     
		currentOriginGo = origin;     
	}
	Direction *currentDir = (Direction *) kmalloc(sizeof(Direction));
	*currentDir = origin;
	if (q_len(vehicleQueue) >= queueSize){
		q_preallocate(vehicleQueue, 100);
		queueSize += 100;
	}
	q_addtail(vehicleQueue, currentDir);
	if (currentOriginGo != origin){
		if (origin == north){
			cv_wait(NGo, intersectionLock);
		}else if (origin == south){
			cv_wait(SGo, intersectionLock);
		}else if (origin == east){
			cv_wait(EGo, intersectionLock);
		}else if (origin == west){
			cv_wait(WGo, intersectionLock);
		}
	}
	lock_release(intersectionLock);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

	void
intersection_after_exit(Direction origin, Direction destination) 
{
	/* replace this default implementation with your own implementation */
	(void)destination; /* avoid compiler complaint about unused parameter */
	KASSERT(vehicleQueue != NULL);
	KASSERT(NGo != NULL);
	KASSERT(SGo != NULL);
	KASSERT(EGo != NULL);
	KASSERT(WGo != NULL);

	lock_acquire(intersectionLock);
	bool found = false;
	int sameCount = 0;
	struct queue *newQueue = q_create(queueSize);

	while(!q_empty(vehicleQueue)){
		if(!found && *((Direction *) q_peek(vehicleQueue)) == origin){
			kfree(q_remhead(vehicleQueue));
			found = true;
		}else{
			if (*((Direction *) q_peek(vehicleQueue)) == origin){
				sameCount++;
			}
			q_addtail(newQueue, q_remhead(vehicleQueue));
		}
	}		
	q_destroy(vehicleQueue);
	vehicleQueue = newQueue;

	if(!q_empty(vehicleQueue) && sameCount == 0){
		Direction newGo = *((Direction *) q_peek(vehicleQueue));
		currentOriginGo = newGo;	
		if (currentOriginGo == north){
			cv_broadcast(NGo, intersectionLock);
		}else if (currentOriginGo == south){
			cv_broadcast(SGo, intersectionLock);
		}else if (currentOriginGo == east){
			cv_broadcast(EGo, intersectionLock);
		}else if (currentOriginGo == west){
			cv_broadcast(WGo, intersectionLock);
		}
	}
	lock_release(intersectionLock);
}
