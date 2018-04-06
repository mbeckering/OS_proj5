/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   user.c
 * Author: Michael Beckering
 * Project 4
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
#define BUFF_SZ sizeof (unsigned int)

// Function prototypes
void initIPC();

// Globals
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
static unsigned int *SC_secs; //pointer to shm sim clock (seconds)
static unsigned int *SC_ns; //pointer to shm sim clock (nanoseconds)

/*
 * 
 */
int main(int argc, char** argv) {
    int rclaim_bound = atoi(argv[1]);
    int my_pnum = atoi(argv[2]);
    
    initIPC();
    
    printf("user%i: terminating: normal\n", my_pnum);
    return (EXIT_SUCCESS);
}

void initIPC() {
    //open semaphore for mutex protection of sim clock
    sem_t *sem = sem_open(SNAME, 0);
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
    
}
