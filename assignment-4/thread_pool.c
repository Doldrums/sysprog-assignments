#include "thread_pool.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct thread_task {
  thread_task_f function;
  void *arg;
  void *result;
  int status;
  bool autodelete;
  struct thread_task *next;
  pthread_cond_t finished;
  pthread_mutex_t busy;
};

struct thread_pool {
  pthread_t *threads;
  int thread_count;
  int max_thread_count;
  int task_count;
  struct thread_task *first;
  struct thread_task *last;
  pthread_mutex_t queue_mutex;
  pthread_cond_t queue_not_empty;
  bool shutdown;
};

void *thread_pool_worker(void *arg) {
  struct thread_pool *pool = arg;

  while (1) {
    pthread_mutex_lock(&pool->queue_mutex);
    while (pool->first == NULL && !pool->shutdown) {
      pthread_cond_wait(&pool->queue_not_empty, &pool->queue_mutex);
    }
    if (pool->shutdown) {
      pthread_mutex_unlock(&pool->queue_mutex);
      return NULL;
    }

    struct thread_task *task = pool->first;

    pool->first = task->next;
    if (pool->first == NULL) {
      pool->last = NULL;
    }
    pthread_mutex_unlock(&pool->queue_mutex);

    pthread_mutex_lock(&task->busy);
    task->status = TTASK_STATUS_RUNNING;
    task->result = task->function(task->arg);
    task->status = TTASK_STATUS_FINISHED;
    pthread_cond_broadcast(&task->finished);
    bool autodelete = task->autodelete;
    pthread_mutex_unlock(&task->busy);

    if (autodelete) {
      pthread_cond_destroy(&task->finished);
      pthread_mutex_destroy(&task->busy);
      free(task);
    }

    pthread_mutex_lock(&pool->queue_mutex);
    pool->task_count--;
    pthread_mutex_unlock(&pool->queue_mutex);
  }

  return NULL;
}

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
  if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }

  struct thread_pool *new_pool = calloc(1, sizeof(struct thread_pool));

  new_pool->max_thread_count = max_thread_count;
  new_pool->thread_count = 0;
  new_pool->task_count = 0;
  new_pool->shutdown = false;
  new_pool->threads = calloc(max_thread_count, sizeof(pthread_t));
  new_pool->first = NULL;
  new_pool->last = NULL;
  pthread_mutex_init(&new_pool->queue_mutex, NULL);
  pthread_cond_init(&new_pool->queue_not_empty, NULL);

  *pool = new_pool;
  return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
  return pool->thread_count;
}

int thread_pool_delete(struct thread_pool *pool) {
  pthread_mutex_lock(&pool->queue_mutex);
  if (pool->task_count != 0) {
    pthread_mutex_unlock(&pool->queue_mutex);
    return TPOOL_ERR_HAS_TASKS;
  }

  pool->shutdown = true;
  pthread_cond_broadcast(&pool->queue_not_empty);
  pthread_mutex_unlock(&pool->queue_mutex);

  for (int i = 0; i < pool->thread_count; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  free(pool->threads);

  pthread_mutex_destroy(&pool->queue_mutex);
  pthread_cond_destroy(&pool->queue_not_empty);

  free(pool);

  return 0;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) {
  if (pool->task_count == TPOOL_MAX_TASKS) {
    return TPOOL_ERR_TOO_MANY_TASKS;
  }

  pthread_mutex_lock(&pool->queue_mutex);
  if (pool->first == NULL) {
    pool->first = task;
    pool->last = task;
  } else {
    pool->last->next = task;
    pool->last = task;
  }
  task->status = TTASK_STATUS_PUSHED;
  task->next = NULL;
  task->autodelete = false;
  pool->task_count++;

  if (pool->task_count > pool->thread_count &&
      pool->thread_count < pool->max_thread_count) {
    pthread_create(&pool->threads[pool->thread_count], NULL, thread_pool_worker,
                   pool);
    pool->thread_count++;
  } else {

    pthread_cond_signal(&pool->queue_not_empty);
  }

  pthread_mutex_unlock(&pool->queue_mutex);

  return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function,
                    void *arg) {
  struct thread_task *new_task = calloc(1, sizeof(struct thread_task));

  new_task->function = function;
  new_task->arg = arg;
  new_task->status = TTASK_STATUS_CREATED;
  new_task->result = NULL;
  new_task->next = NULL;
  new_task->autodelete = false;
  pthread_mutex_init(&new_task->busy, NULL);
  pthread_cond_init(&new_task->finished, NULL);

  *task = new_task;

  return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
  return task->status == TTASK_STATUS_FINISHED ||
         task->status == TTASK_STATUS_JOINED;
}

bool thread_task_is_running(const struct thread_task *task) {
  return task->status == TTASK_STATUS_RUNNING;
}

int thread_task_join(struct thread_task *task, void **result) {
  pthread_mutex_lock(&task->busy);

  if (task->status == TTASK_STATUS_CREATED) {
    pthread_mutex_unlock(&task->busy);
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  while (task->status != TTASK_STATUS_FINISHED &&
         task->status != TTASK_STATUS_JOINED) {
    pthread_cond_wait(&task->finished, &task->busy);
  }
  task->status = TTASK_STATUS_JOINED;
  pthread_mutex_unlock(&task->busy);

  *result = task->result;

  return 0;
}

int thread_task_timed_join(struct thread_task *task, double timeout,
                           void **result) {
  if (task->status == TTASK_STATUS_CREATED) {
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  timeout += 10e-7;
  struct timespec abs_timeout;
  clock_gettime(CLOCK_MONOTONIC, &abs_timeout);
  abs_timeout.tv_sec += (int)timeout;
  abs_timeout.tv_nsec += (timeout - (int)timeout) * 1e9;

  pthread_mutex_lock(&task->busy);
  while (task->status != TTASK_STATUS_FINISHED &&
         task->status != TTASK_STATUS_JOINED) {
    if (pthread_cond_timedwait(&task->finished, &task->busy, &abs_timeout) ==
        ETIMEDOUT) {
      pthread_mutex_unlock(&task->busy);
      return TPOOL_ERR_TIMEOUT;
    }
  }
  task->status = TTASK_STATUS_JOINED;
  pthread_mutex_unlock(&task->busy);

  *result = task->result;

  return 0;
}

int thread_task_delete(struct thread_task *task) {
  pthread_mutex_lock(&task->busy);
  if (task->status != TTASK_STATUS_CREATED &&
      task->status != TTASK_STATUS_JOINED) {
    pthread_mutex_unlock(&task->busy);
    return TPOOL_ERR_TASK_IN_POOL;
  }
  pthread_mutex_unlock(&task->busy);

  pthread_cond_destroy(&task->finished);
  pthread_mutex_destroy(&task->busy);
  free(task);

  return 0;
}

#ifdef NEED_DETACH

int thread_task_detach(struct thread_task *task) {
  pthread_mutex_lock(&task->busy);
  if (task->status == TTASK_STATUS_CREATED) {
    pthread_mutex_unlock(&task->busy);
    return TPOOL_ERR_TASK_NOT_PUSHED;
  } else if (task->status == TTASK_STATUS_FINISHED) {
    pthread_mutex_unlock(&task->busy);
    pthread_cond_destroy(&task->finished);
    pthread_mutex_destroy(&task->busy);
    free(task);
    return 0;
  }
  task->autodelete = true;
  pthread_mutex_unlock(&task->busy);

  return 0;
}

#endif
