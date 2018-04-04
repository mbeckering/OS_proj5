/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   oss.c
 * Author: Michael Beckering
 * Project 5
 * Spring 2018 CS-4760-E01
 * Created on April 14, 2018, 10:12 AM
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
#define BUFF_SZ sizeof (unsigned int)

// Function prototypes
void printAvail();
void initIPC();
void clearIPC();

// GLOBALS
sem_t *sem; //sim clock mutual exclusion
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int rclaim_bound = 3; // upper bound for maximum resource claim by users
int bitVector[18]; // keeps track of what simulated pids are running
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)

struct state {
    int resource[20]; //total system resources initially available
    int avail[20]; //number of available resources of each type (types 0-19)
    int max_claim[18][20]; //max claimable resource demand of each user process
    int alloc[18][20]; //resources of each type currently allocated to each user
    //int need[18][20]; //max - allocation: possible remaining need of each user
};
struct state liveState;
//struct state testState;

int safe () {
    int i=0, j=0, k=0, possible=0, found=0;
    int need[P][R]; //max additional needs of each resource by each process
    int pcount = numUsersRunning();
    int processes[pcount];
    //put running pids into process array
    for (i=0; i<P; i++) {
        if (bitVector[i] == 1) {
            processes[j] = i;
            j++;
        }
    }
    //copy currently available resources into local avail array
    int currentavail[R];
    for (i=0; i<R; i++) {
        currentavail[i] = liveState.avail[i];
    }
    //find max additional needs of each resource by each process
    for (i=0; i<P; i++) {
        for (j=0; j<R; j++) {
            need[i][j] = liveState.max_claim[i][j] - liveState.alloc[i][j];
        }
    }
    possible = 1;
    
    while (possible) {
        //for each process in the array of running processes
        for (i=0; i<pcount; i++) {
            //and for each resource type for each running process
            for (j=0; j<R; j++) {
                //if this process could claim every resource it could
                //possibly need from currently available resources
                
                }
            }
        }
    }
    
    
}

int numUsersRunning() {
    int i = 0;
    int pcount = 0;
    for (i=0; i<P; i++) {
        if (bitVector[i] == 1) {
            pcount++;
        }
    }
    return pcount;
}

/*
 * 
 */
int main(int argc, char** argv) {
    char str_arg1[10]; //strings for execl call args
    char str_arg2[10];
    int next_pnum = 0; //next process number to launch
    pid_t childpid; //child pid
    int i, j; // iterators
    int status;
    int seed = getpid();
    
    // Determine initial available resources
    for (j=0; j<R; j++) {
        avail[j] = rand_r(&seed) % 10 + 1;
    }
    
    initIPC();
    
    printAvail();
    if ( (childpid = fork()) < 0 ){ //terminate code
        perror("OSS: Error forking user");
        clearIPC();
        exit(0);
    }
    if (childpid == 0) { //child code
        sprintf(str_arg1, "%i", rclaim_bound); //build arg1 string
        sprintf(str_arg2, "%i", next_pnum); //build arg2 string
        execlp("./user", "./user", str_arg1, str_arg1, (char *)NULL);
        perror("OSS: execl() failure"); //report & exit if exec fails
        exit(0);
    }
    

    pid_t waitpid;
    while( (waitpid = wait(&status)) > 0);
    clearIPC();
    printf("OSS: normal exit\n");
    return (EXIT_SUCCESS);
}

void initIPC() {
    //create semaphore for mutex protection of sim clock
    sem = sem_open(SNAME, O_CREAT, 0777, 1);
    
    //sim clock seconds
    shmid_sim_secs = shmget(SHMKEY_sim_s, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_secs == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_secs");
            exit(1);
        }
    SC_secs = (unsigned int*) shmat(shmid_sim_secs, 0, 0);
    //sim clock nanoseconds
    shmid_sim_ns = shmget(SHMKEY_sim_ns, BUFF_SZ, 0777 | IPC_CREAT);
        if (shmid_sim_ns == -1) { //terminate if shmget failed
            perror("OSS: error in shmget shmid_sim_ns");
            exit(1);
        }
    SC_ns = (unsigned int*) shmat(shmid_sim_ns, 0, 0);
}

void printAvail() {
    int i;
    for(i=0; i<R; i++){
        printf("P%i: %i ", i, avail[i]);
    }
    printf("\n");
}

void clearIPC() {
    printf("OSS: Clearing IPC resources...\n");
    //close the semaphore
    if (sem_unlink(SNAME) == -1) {
        perror("OSS: Error unlinking semaphore");
    }
    if (sem_close(sem) == -1) {
        perror("OSS: Error closing semaphore");
    }
    //close shared memory (sim clock)
    if ( shmctl(shmid_sim_secs, IPC_RMID, NULL) == -1) {
        perror("error removing shared memory");
    }
    if ( shmctl(shmid_sim_ns, IPC_RMID, NULL) == -1) {
        perror("error removing shared memory");
    }
}
