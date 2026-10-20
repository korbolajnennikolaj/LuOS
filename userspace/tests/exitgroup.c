// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static volatile unsigned long spins[4];
static void *spinner(void *a) { int i = (int)(long)a; for (;;) spins[i]++; return NULL; }
static void *waiter(void *a) { (void)a; pthread_mutex_lock(&m); for (;;) pthread_cond_wait(&c, &m); return NULL; }
static void *sleeper(void *a) { (void)a; for (;;) sleep(100); return NULL; }
int main(void) {
    pthread_t t;
    for (long i = 0; i < 4; i++) pthread_create(&t, NULL, spinner, (void *)i);
    for (int i = 0; i < 2; i++) pthread_create(&t, NULL, waiter, NULL);
    pthread_create(&t, NULL, sleeper, NULL);
    struct timespec ts = { 0, 300000000 };
    nanosleep(&ts, NULL);
    printf("spinners ran %lu %lu %lu %lu loops; exiting with 3 while 7 threads are busy\n", spins[0], spins[1], spins[2], spins[3]);
    exit(3);
}
