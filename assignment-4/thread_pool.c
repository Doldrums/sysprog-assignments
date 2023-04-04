#include "thread_pool.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct thread_task {
  thread_task_f function;
  void *arg;
  void *result;
  int status;
  struct thread_task *next;
  pthread_cond_t finished;
};

struct thread_pool {
  pthread_t *threads;
  int thread_count;
  int free_threads;
  int max_thread_count;
  int task_count;
  struct thread_task *first;
  struct thread_task *last;
  pthread_mutex_t queue_mutex;
  pthread_cond_t queue_not_empty;
  pthread_mutex_t free_threads_mutex;
};

void *thread_pool_worker(void *arg) {
  struct thread_pool *pool = arg;

  while (1) {
    pthread_mutex_lock(&pool->free_threads_mutex);
    pool->free_threads++;
    pthread_mutex_unlock(&pool->free_threads_mutex);

    struct thread_task *task;
    pthread_mutex_lock(&pool->queue_mutex);
    while (pool->first == NULL) {
      pthread_cond_wait(&pool->queue_not_empty, &pool->queue_mutex);
    }

    pthread_mutex_lock(&pool->free_threads_mutex);
    pool->free_threads--;
    pthread_mutex_unlock(&pool->free_threads_mutex);

    task = pool->first;
    pool->first = task->next;
    if (pool->first == NULL) {
      pool->last = NULL;
    }
    pool->task_count--;
    pthread_mutex_unlock(&pool->queue_mutex);

    task->status = TTASK_STATUS_RUNNING;
    task->result = task->function(task->arg);
    task->status = TTASK_STATUS_FINISHED;
    pthread_cond_broadcast(&task->finished);
  }

  return NULL;
}

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
  if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }

  struct thread_pool *new_pool = malloc(sizeof(struct thread_pool));

  new_pool->max_thread_count = max_thread_count;
  new_pool->thread_count = 0;
  new_pool->free_threads = 0;
  new_pool->task_count = 0;
  new_pool->threads = malloc(max_thread_count * sizeof(pthread_t));
  new_pool->first = NULL;
  new_pool->last = NULL;
  pthread_cond_init(&new_pool->queue_not_empty, NULL);
  pthread_mutex_init(&new_pool->queue_mutex, NULL);
  pthread_mutex_init(&new_pool->free_threads_mutex, NULL);

  *pool = new_pool;
  return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
  return pool->thread_count;
}

int thread_pool_delete(struct thread_pool *pool) {
  if (pool->task_count != 0) {
    return TPOOL_ERR_HAS_TASKS;
  }

  for (int i = 0; i < pool->thread_count; i++) {
    pthread_cancel(pool->threads[i]);
  }

  free(pool->threads);

  pthread_mutex_destroy(&pool->queue_mutex);
  pthread_mutex_destroy(&pool->free_threads_mutex);
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
  pthread_cond_init(&task->finished, NULL);
  pool->task_count++;
  pthread_cond_broadcast(&pool->queue_not_empty);
  pthread_mutex_unlock(&pool->queue_mutex);

  if (pool->free_threads == 0 && pool->thread_count < pool->max_thread_count) {
    pthread_create(&pool->threads[pool->thread_count++], NULL,
                   thread_pool_worker, pool);
  }

  return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function,
                    void *arg) {
  struct thread_task *new_task = malloc(sizeof(struct thread_task));

  new_task->function = function;
  new_task->arg = arg;
  new_task->status = TTASK_STATUS_CREATED;
  new_task->result = NULL;
  new_task->next = NULL;

  *task = new_task;

  return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
  return task->status == TTASK_STATUS_FINISHED;
}

bool thread_task_is_running(const struct thread_task *task) {
  return task->status == TTASK_STATUS_RUNNING;
}

int thread_task_join(struct thread_task *task, void **result) {
  if (task->status == TTASK_STATUS_CREATED) {
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  pthread_mutex_t finished_mutex;
  pthread_mutex_init(&finished_mutex, NULL);
  pthread_mutex_lock(&finished_mutex);
  while (task->status != TTASK_STATUS_FINISHED) {
    pthread_cond_wait(&task->finished, &finished_mutex);
  }
  pthread_mutex_unlock(&finished_mutex);
  pthread_mutex_destroy(&finished_mutex);

  *result = task->result;

  return 0;
}

int thread_task_join2(struct thread_task *task, double timeout, void **result) {
  if (task->status == TTASK_STATUS_CREATED) {
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  struct timespec abs_timeout;
  clock_gettime(CLOCK_REALTIME, &abs_timeout);
  abs_timeout.tv_sec += (int)timeout;
  abs_timeout.tv_nsec += (timeout - (int)timeout) * 1e9;

  pthread_mutex_t finished_mutex;
  pthread_mutex_init(&finished_mutex, NULL);

  pthread_mutex_lock(&finished_mutex);
  while (task->status != TTASK_STATUS_FINISHED) {
    if (pthread_cond_timedwait(&task->finished, &finished_mutex,
                               &abs_timeout) == ETIMEDOUT) {
      pthread_mutex_unlock(&finished_mutex);
      return TPOOL_ERR_TIMEOUT;
    }
  }
  pthread_mutex_unlock(&finished_mutex);

  *result = task->result;

  return 0;
}

int thread_task_delete(struct thread_task *task) {
  if (task->status == TTASK_STATUS_PUSHED ||
      task->status == TTASK_STATUS_RUNNING) {
    return TPOOL_ERR_TASK_IN_POOL;
  }

  pthread_cond_destroy(&task->finished);
  free(task);

  return 0;
}

#ifdef NEED_DETACH

int thread_task_detach(struct thread_task *task) {
  if (task->status == TTASK_STATUS_CREATED) {
    return TPOOL_ERR_TASK_NOT_PUSHED;
  } else if (task->status == TTASK_STATUS_FINISHED) {
    thread_task_delete(task);
  }

  void *result;
  thread_task_join(task, &result);
  free(result);

  return 0;
}

#endif
