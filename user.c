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
void initIPC(); //Initialize IPC resources
void setMaxClaims(int, int); //decide this user's max claim for each resource
void incrementClock(unsigned int, unsigned int); //add time to sim clock

// Globals
int seed; //seed for random rolls
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_state; //shared memory ID holder for the liveState struct
int shmid_qid; //shared memory ID holder for message queue
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)
sem_t *sem; //sim clock mutual exclusion

//struct for shared memory, holds information about the state of the system
struct state {
    int resource[20]; //total system resources initially available
    int avail[20]; //number of available resources of each type (types 0-19)
    int max_claim[18][20]; //max claimable resource demand of each user process
    int alloc[18][20]; //resources of each type currently allocated to each user
    //int need[18][20]; //max - allocation: possible remaining need of each user
};
struct state *liveState;
struct state liveStateStruct;

/*
 * 
 */
int main(int argc, char** argv) {
    int rclaim_bound = atoi(argv[1]);
    int my_pnum = atoi(argv[2]);
    seed = getpid();
    
    initIPC();
    setMaxClaims(my_pnum, rclaim_bound);
    
    printf("USER: max bound: %i\n", rclaim_bound);
    int i;
    printf("USER:  R0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\nresource:  ");
    for(i=0; i<R; i++){
        printf("%i\t", (*liveState).resource[i]);
    }
    printf("\n");
    
    printf("USER:  R0 \tR1 \tR2 \tR3 \tR4 \tR5 \tR6 \tR7 \tR8 \tR9 \tR10"
    "\tR11\tR12\tR13\tR14\tR15\tR16\tR17\tR18\tR19\nmax_claim:  ");
    for(i=0; i<R; i++){
        printf("%i\t", (*liveState).max_claim[my_pnum][i]);
    }
    printf("\n");
    
    printf("user%i: terminating: normal\n", my_pnum);
    return (EXIT_SUCCESS);
}

void incrementClock(unsigned int add_secs, unsigned int add_ns) {
    sem_wait(sem); //mutex protection
    unsigned int localsec = *SC_secs;
    unsigned int localns = *SC_ns;
    unsigned int temp;
    localsec = localsec + add_secs;
    localns = localns + add_ns;
    //rollover nanoseconds offset if needed
    if (localns >= BILLION) {
        localsec++;
        temp = localns - BILLION;
        localns = temp;
    }
    //update the sim clock in shared memory
    *SC_secs = localsec;
    *SC_ns = localns;
    sem_post(sem);
}

void setMaxClaims(int my_pnum, int max_bound) {
    int r; //resource # iterator
    max_bound += 1; //actually roll 0-bound instead of 0-(bound-1)
    for (r=0; r<R; r++) {
        //if total system resource type r is less than max bound,
        //roll my max up to that total instead, to avoid immediate unsafe state
        if ((*liveState).resource[r] < max_bound) {
            (*liveState).max_claim[my_pnum][r] = rand_r(&seed) 
                    % (*liveState).resource[r];
        }
        //otherwise roll from 0 to max bound set by oss, retrived by execl arg
        else {
            (*liveState).max_claim[my_pnum][r] = rand_r(&seed) % max_bound;
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
    if ( (shmid_qid = msgget(SHMKEY_msgq, 0777 | IPC_CREAT)) == -1 ) {
        perror("OSS: Error generating message queue");
        exit(0);
    }
    
    
}
