/* 
 * File:   user.c
 * Author: Michael Beckering
 * Project 5
 * Spring 2018 CS-4760-E01
 * Created on April 4, 2018, 10:14 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <pthread.h>

#include <semaphore.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SNAME "clocksem"
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define SHMKEY_msgq 4020015
#define BUFF_SZ sizeof (unsigned int)
#define SHMKEY_state 4020014
#define BUFF_SZ sizeof (unsigned int)
#define BILLION 1000000000

// Function prototypes
void terminate();
void setRequest();
void setRelease();
int haveAnyResources();
void setTimeToNextEvent();
int isTimeForEvent();
int roll1000();
void initIPC(); //Initialize IPC resources
void setMaxClaims(int, int); //decide this user's max claim for each resource
void incrementClock(unsigned int, unsigned int); //add time to sim clock

// Globals
int my_pnum;
int seed; //seed for random rolls
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_state; //shared memory ID holder for the liveState struct
int shmid_qid; //shared memory ID holder for message queue
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)
unsigned int nextEventSecs, nextEventNS;
unsigned int ns_between_events;
sem_t *sem; //sim clock mutual exclusion

//struct for shared memory, holds information about the state of the system
struct state {
    int resource[20]; //total system resources initially available
    int avail[20]; //number of available resources of each type (types 0-19)
    int max_claim[18][20]; //max claimable resource demand of each user process
    int alloc[18][20]; //resources of each type currently allocated to each user
    int pending_req[18][20]; //pending requests (to store if blocked)
    //int need[18][20]; //max - allocation: possible remaining need of each user
};
struct state *liveState;
struct state liveStateStruct;

struct commsbuf {
    long msgtyp;
    int user_sim_pid;
    pid_t user_sys_pid;
    int r_type; //resource type
    int r_qty; //resource quantity
    int user_requesting;//
    int user_terminating; //from user. 1=terminating, 0=not terminating
    int user_denied; //from oss. 1=resources denied (put into blocked queue)
    int user_granted; //from oss. 1=resource request granted
    int user_releasing; //from user. 1=releasing resources r_qty, r_type
};

struct commsbuf msg;

/*
 * 
 */
int main(int argc, char** argv) {
    int rclaim_bound = atoi(argv[1]);
    my_pnum = atoi(argv[2]);
    int i, roll;
    seed = getpid();
    ns_between_events = (unsigned int)rand_r(&seed) % 500000 + 1;
    
    initIPC();
    setMaxClaims(my_pnum, rclaim_bound);
    
    //determine time to initiate first event
    setTimeToNextEvent();
    while(1) {
        //if it's time for an event
        if (isTimeForEvent()) {
            roll = roll1000();
            //rolled to request
            if (roll > 500) {
                //decide type & quantity to request
                setRequest();
                //send message to request
                if (msgsnd(shmid_qid, &msg, sizeof(msg), 0) == -1 ) {
                    perror("User: error sending msg to oss");
                    exit(0);
                }
                //WAIT for response before doing anything else
                while(1) {
                    if (msgrcv(shmid_qid, &msg, sizeof(msg), (my_pnum + 100), 0) == -1 ) {
                        perror("User: error in msgrcv");
                        exit(1);
                    }
                    printf("User%i: message received, granted=%i\n", my_pnum, msg.user_granted);
                    //if request is granted, roll to terminate
                    if (msg.user_granted == 1) {
                        roll = roll1000();
                        if (roll > 950) {
                            terminate();
                        }
                        //if request was granted but didn't terminate, keep looping
                        else break;
                    }
                    else {
                        printf("User%i: blocked and waiting to be awoken\n", my_pnum);
                    }
                }
            }
            //if I roll to release AND I have resources to release
            else if ( haveAnyResources() ) {
                //decide what resource to release
                setRelease();
                //send message to release
                if ( msgsnd(shmid_qid, &msg, sizeof(msg), 0) == -1 ) {
                    perror("User: error sending msg to oss");
                    exit(0);
                }
            }
            setTimeToNextEvent();
        }
    }
  
    printf("user%i: terminating: normal\n", my_pnum);
    return (EXIT_SUCCESS);
}

void terminate() {
    //send message to OSS that I'm terminating
    msg.msgtyp = 99;
    msg.user_releasing = 0;
    msg.user_requesting = 0;
    msg.user_sim_pid = my_pnum;
    msg.user_sys_pid = getpid();
    msg.user_terminating = 1;
    if ( msgsnd(shmid_qid, &msg, sizeof(msg), 0) == -1 ) {
        perror("User: error sending msg to oss");
        exit(0);
    }
    printf("User%i rolled to terminate, message sent\n", my_pnum);
    exit(0);
}

void setRequest() {
    int r, picktype, maxrequest;
    int repeat = 1;
    while (repeat) {
        //select a resource type at random
        picktype = rand_r(&seed) % R;
        //if this user hasn't used its max claim of that type
        if( (*liveState).alloc[my_pnum][picktype] < (*liveState).max_claim[my_pnum][picktype]) {
            //set max request to max_claim - what I already have allocated to me
            maxrequest = (*liveState).max_claim[my_pnum][picktype] 
                    - (*liveState).alloc[my_pnum][picktype];
            //set up our message for requesting this resource
            msg.msgtyp = 99;
            //roll a random request amount from 1 to maxrequest
            msg.r_qty = rand_r(&seed) % maxrequest + 1;
            msg.r_type = picktype;
            msg.user_releasing = 0;
            msg.user_requesting = 1;
            msg.user_sim_pid = my_pnum;
            msg.user_terminating = 0;
            repeat = 0;
        }
    }
}

void setRelease() {
    int r, picktype, rollin;
    int found = 0;
    //pick a random location to start from in MY row of the allocation matrix
    rollin = rand_r(&seed) % 18;
    //go through the resource types starting at that location
    for (r=rollin; r<R; r++) {
        //if this user has any of this type allocated
        if ((*liveState).alloc[my_pnum][r] > 0) {
            found = 1;
            picktype = r;
        }
        if (found == 1) break;
    }
    //if a resource wasn't found from the random starting location,
    //start from beginning of my allocation row and look from there
    if (found == 0) {
        for (r=0; r<R; r++) {
            if ((*liveState).alloc[my_pnum][r] > 0) {
            found = 1;
            picktype = r;
        }
        if (found == 1) break;
        }
    }
    //set up our message for requesting this resource
    msg.msgtyp = 99;
    //roll a random amount to release from 1 to current allocation
    msg.r_qty = rand_r(&seed) % (*liveState).alloc[my_pnum][picktype] +1;
    msg.r_type = picktype;
    msg.user_releasing = 1;
    msg.user_requesting = 0;
    msg.user_sim_pid = my_pnum;
    msg.user_terminating = 0;
    printf("User%i: msg to OSS: releasing %i of rtype %i\n", my_pnum, msg.r_qty, msg.r_type);
}

int haveAnyResources () {
    int return_val = 0;
    int r;
    for (r=0; r<R; r++) {
        //if this user has a claim on any resource, return 1
        if ( (*liveState).alloc[my_pnum][r] != 0 ) {
            return_val = 1;
            break;
        }
    }
    return return_val;
}

void setTimeToNextEvent() {
    unsigned int temp;
    sem_wait(sem);
    unsigned int localsecs = *SC_secs;
    unsigned int localns = *SC_ns;
    sem_post(sem);
    nextEventSecs = localsecs;
    nextEventNS = localns + ns_between_events;
    if (nextEventNS >= BILLION) { //roll ns to s if > bill
        nextEventSecs++;
        temp = nextEventNS - BILLION;
        nextEventNS = temp;
    }
}

int isTimeForEvent() {
    int return_val = 0;
    sem_wait(sem);
    unsigned int localsec = *SC_secs;
    unsigned int localns = *SC_ns;
    sem_post(sem);
    if ( (localsec > nextEventSecs) || 
            ( (localsec >= nextEventSecs) && (localns >= nextEventNS) ) ) {
        return_val = 1;
    }
    return return_val;
}

int roll1000() {
    int return_val;
    return_val = rand_r(&seed) % 1000 + 1;
    return return_val;
}

void setMaxClaims(int my_pnum, int max_bound) {
    int r; //resource # iterator
    //max_bound += 1; //actually roll 0-bound instead of 0-(bound-1)
    for (r=0; r<R; r++) {
        //if total system resource type r is less than max bound,
        //roll my max up to that total instead, to avoid immediate unsafe state
        if ((*liveState).resource[r] < max_bound) {
            (*liveState).max_claim[my_pnum][r] = rand_r(&seed) 
                    % (*liveState).resource[r] + 1;
        }
        //otherwise roll from 0 to max bound set by oss, retrived by execl arg
        else {
            (*liveState).max_claim[my_pnum][r] = rand_r(&seed) % max_bound + 1;
        }
    }
}

void initIPC() {
    //open semaphore for mutex protection of sim clock
    sem = sem_open(SNAME, 0);
    //sim clock: seconds
    shmid_sim_secs = shmget(SHMKEY_sim_s, BUFF_SZ, 0777);
        if (shmid_sim_secs == -1) { //terminate if shmget failed
            perror("User: error in shmget shmid_sim_secs");
            exit(1);
        }
    SC_secs = (unsigned int*) shmat(shmid_sim_secs, 0, 0);
    //sim clock: nanoseconds
    shmid_sim_ns = shmget(SHMKEY_sim_ns, BUFF_SZ, 0777);
        if (shmid_sim_ns == -1) { //terminate if shmget failed
            perror("User: error in shmget shmid_sim_ns");
            exit(1);
        }
    SC_ns = (unsigned int*) shmat(shmid_sim_ns, 0, 0);
    //shared memory for system state struct
    shmid_state = shmget(SHMKEY_state, sizeof(struct state), 0777);
    if (shmid_state == -1) { //terminate if shmget failed
            perror("OSS: error in shmget state");
            exit(1);
        }
    liveState = (struct state *) shmat(shmid_state, NULL, 0);
    if (liveState == (struct state*)(-1) ) {
        perror("OSS: error in shmat liveState");
        exit(1);
    }
    //message queue
    if ( (shmid_qid = msgget(SHMKEY_msgq, 0777)) == -1 ) {
        perror("OSS: Error generating message queue");
        exit(0);
    }
    
    
}
