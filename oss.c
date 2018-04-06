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
#include <string.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>

#include <semaphore.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SNAME "clocksem"
#define SHMKEY_sim_s 4020012
#define SHMKEY_sim_ns 4020013
#define BUFF_SZ sizeof (unsigned int)
#define BILLION 1000000000

// Function prototypes
static int setperiodic(double);
static int setinterrupt();
static void interrupt(int signo, siginfo_t *info, void *context);
static void siginthandler(int sig_num);

void printAvail();
void initIPC();
void clearIPC();
int safe();
int numUsersRunning(int[]);

// GLOBALS
sem_t *sem; //sim clock mutual exclusion
int R = 20; // Number of resource types
int P = 18; // Max number of user processes
int rclaim_bound = 3; // upper bound for maximum resource claim by users
int bitVector[18]; // keeps track of what simulated pids are running
int shmid_sim_secs, shmid_sim_ns; //shared memory ID holders for sim clock
int shmid_state; //shared memory ID holder for the liveState struct*****************************
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


/********************************* MAIN ***************************************/
int main(int argc, char** argv) {
    char str_arg1[10]; //strings for execl call args
    char str_arg2[10];
    int option; //option int for getopt args
    int verbose = 0; //option for verbose mode
    int next_pnum = 0; //next process number to launch
    double runtime = 2; //runtime limit
    pid_t childpid; //child pid
    int i, j; //iterators
    int status;
    int seed = getpid();
    
    signal (SIGINT, siginthandler);
    
    if (setperiodic(runtime) == -1) {
        perror("Failed to setup periodic interrupt");
        return 1;
    }
    
    while ((option = getopt(argc, argv, "hv")) != -1) {
        switch(option) {
            case 'h':
                printf("Usage: ./oss [-v]\n");
                printf("-v: verbose mode on\n");
                exit (0);
                break;
            case 'v':
                verbose = 1;
                printf("OSS: verbose mode ON\n");
                break;
            default:
                break;
        }
    }
    if (verbose == 0) {
        printf("OSS: verbose mode OFF\n");
    }
    
    // Determine initial available resources
    for (j=0; j<R; j++) {
        liveState.avail[j] = rand_r(&seed) % 10 + 1;
    }
    
    initIPC();
    
    //printAvail();
    if ( (childpid = fork()) < 0 ){ //terminate code
        perror("OSS: Error forking user");
        clearIPC();
        exit(0);
    }
    if (childpid == 0) { //child code
        sprintf(str_arg1, "%i", rclaim_bound); //build arg1 string
        sprintf(str_arg2, "%i", next_pnum); //build arg2 string
        execlp("./user", "./user", str_arg1, str_arg2, (char *)NULL);
        perror("OSS: execl() failure"); //report & exit if exec fails
        exit(0);
    }
    bitVector[5] = 1;
    bitVector[0] = 1;
    bitVector[15] = 1;
    bitVector[16] = 1;
    bitVector[17] = 1;
    int poop = safe();
    printf("OSS: poop = %i\n", poop);
    pid_t waitpid;
    while( (waitpid = wait(&status)) > 0);
    clearIPC();
    printf("OSS: normal exit\n");
    return (EXIT_SUCCESS);
}

/******************************* FUNCTIONS ************************************/

//Function to determine if the system is in a safe state (banker's algorithm)
int safe () {
    int i=0, j=0, k=0, possible=0, found=0, count=0;
    int need[P][R]; //max additional needs of each resource by each process
    int pcount = numUsersRunning(bitVector);
    printf("pcount (numUsersRunning) = %i\n", pcount);
    //copy currently available resources into local avail array
    int currentavail[R];
    printf("avail: ");
    for (i=0; i<R; i++) {
        currentavail[i] = liveState.avail[i];
        printf("R%i: %i, ", i, liveState.avail[i]);
    }
    printf("\n");
    int testVector[P];
    for (i=0; i<P; i++) {
        testVector[i] = bitVector[i];
    }
    //find max additional needs of each resource by each process
    for (i=0; i<P; i++) {
        for (j=0; j<R; j++) {
            need[i][j] = liveState.max_claim[i][j] - liveState.alloc[i][j];
        }
    }
    possible = 1;
    
    while (count < P) {
        found = 0;
        currentavail[5] = 1;
        need[15][5] = 1;
        //for each process
        for (i=0; i<P; i++) {
            //that is RUNNING in our system copy
            if (testVector[i] == 1) {
                printf("Looking at user pid %i\n", i);
                //and for each resource type for this running process
                for (j=0; j<R; j++) {
                    //if this process could claim every resource it could
                    //possibly need from currently available resources
                    if (need[i][j] <= currentavail[j]) {
                        printf("user pid %i max need %i of resource %i and currentavail: %i\n",
                                i, need[i][j], j, currentavail[j]);
                        //if this loop made it to R-1, then this could be
                        //granted all resources it needs to run to completion
                        if (j == R-1) {
                            found = 1;
                            count++; //move toward exiting while loop
                            //simulate termination of this process
                            testVector[i] = 0;
                            //increment sim available resources we would gain
                            //from termination of this process
                            for (k=0; k<R; k++){
                                currentavail[k] = currentavail[k] 
                                        + liveState.alloc[i][k];
                            }
                            //i=1000; //this will jump back to beginning of while
                            //j=1000; //loop and start again without this process
                        }
                    }
                    //go on to next process if there aren't enough available
                    //resources to satisfy all of this user's possible needs
                    else break;
                }//end resource for loop
            }//end running process if statement
        } //end process for loop
        pcount = numUsersRunning(testVector);
        if (found == 1 && pcount == 0) {
            break;
        }
        else if (found == 0) {
            printf("OSS: System not in safe state\n");
            return 0;
        }
    }
    printf("OSS: System is in safe state\n");
    return 1;
}

int numUsersRunning(int vector[]) {
    int i = 0;
    int pcount = 0;
    for (i=0; i<P; i++) {
        if (vector[i] == 1) {
            pcount++;
        }
    }
    return pcount;
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
        printf("R%i: %i ", i, liveState.avail[i]);
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

/************************* INTERRUPT HANDLING *********************************/
//this function taken from UNIX text
static int setperiodic(double sec) {
    timer_t timerid;
    struct itimerspec value;
    
    if (timer_create(CLOCK_REALTIME, NULL, &timerid) == -1)
        return -1;
    value.it_interval.tv_sec = (long)sec;
    value.it_interval.tv_nsec = (sec - value.it_interval.tv_sec)*BILLION;
    if (value.it_interval.tv_nsec >= BILLION) {
        value.it_interval.tv_sec++;
        value.it_interval.tv_nsec -= BILLION;
    }
    value.it_value = value.it_interval;
    return timer_settime(timerid, 0, &value, NULL);
}

//this function taken from UNIX text
static int setinterrupt() {
    struct sigaction act;
    
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = interrupt;
    if ((sigemptyset(&act.sa_mask) == -1) ||
            (sigaction(SIGALRM, &act, NULL) == -1))
        return -1;
    return 0;
}

static void interrupt(int signo, siginfo_t *info, void *context) {
    printf("Master: Timer Interrupt Detected! signo = %d\n", signo);
    //killchildren();
    clearIPC();
    //close log file
    //fprintf(mlog, "Master: Terminated: Timed Out\n");
    //fclose(mlog);
    printf("Master: Terminated: Timed Out\n");
    exit(0);
}

static void siginthandler(int sig_num) {
    //printf("Master pid=%ld: Ctrl+C interrupt detected! signo = %d\n", getpid(), sig_num);
    
    //killchildren();
    clearIPC();
    
    //fprintf(mlog, "Master: Terminated: Interrupted\n");
    //fclose(mlog);
    
    printf("Master: Terminated: Interrupted\n");
    exit(0);
}