#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libcoro.h"

typedef struct files {
	const char *name;
	int *sorted;
	int count;

	struct files *next;
} files;

static files *file_queue = NULL;
static files *queue_pointer = NULL;

uint64_t target_latency;
uint64_t* last_yield;
uint64_t* time_taken;

size_t pool_size;

long get_current_time_in_microseconds()
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (long) (tp.tv_sec * 1000000 + tp.tv_nsec / 1000);
}

void swap(int* a, int* b) 
{ 
    int t = *a; 
    *a = *b; 
    *b = t; 
} 

int partition(int* arr, int low, int high) 
{ 
    int pivot = arr[high];
    int i = (low - 1);
  
    for (int j = low; j <= high- 1; j++) 
    { 
        if (arr[j] <= pivot) 
        { 
            i++;
            swap(&arr[i], &arr[j]); 
        } 
    } 
    swap(&arr[i + 1], &arr[high]); 
    return (i + 1); 
}

void quick_sort(int* arr, int low, int high, int coro_id) 
{ 
    if (low < high) 
    { 
        int pi = partition(arr, low, high); 

		long cur_time = get_current_time_in_microseconds();
		if (cur_time - last_yield[coro_id] > target_latency) {
			time_taken[coro_id] += cur_time - last_yield[coro_id];
			coro_yield();
			last_yield[coro_id] = get_current_time_in_microseconds();
		}

        quick_sort(arr, low, pi - 1, coro_id); 
        quick_sort(arr, pi + 1, high, coro_id); 
    } 
} 

void print_array(int* arr, int size) {
    for (int i = 0; i < size; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
}

static int coroutine_func_f(void *context)
{
	struct coro *this = coro_this();
	int* id = context;

	last_yield[*id] = get_current_time_in_microseconds();

	while (queue_pointer != NULL) {
		struct files *file = queue_pointer;
		queue_pointer = queue_pointer->next;

		FILE* fp = fopen(file->name, "r");
		int num;
		int cnt = 0;
		while (fscanf(fp, "%d", &num) != EOF) {
			cnt++;
		}
		int* numbers = calloc(cnt, sizeof(int));
		rewind(fp);

		cnt = 0;
		while (fscanf(fp, "%d", &num) != EOF) {
			numbers[cnt++] = num;
		}

		quick_sort(numbers, 0, cnt - 1, *id);

		file->sorted = numbers;
		file->count = cnt;
		fclose(fp);
	}

	time_taken[*id] += get_current_time_in_microseconds() - last_yield[*id];
	printf("coroutine %d finsihed with %lld switches and executing time %llu microseconds\n", *id, coro_switch_count(this), time_taken[*id]);

	free(context);
	return 0;
}

int main(int argc, char **argv) {
    uint64_t t = get_current_time_in_microseconds(); 

	pool_size = strtol(argv[2], NULL, 10);
	target_latency = strtol(argv[1], NULL, 10) / pool_size;

	last_yield = calloc(pool_size, sizeof(uint64_t));
	time_taken = calloc(pool_size, sizeof(uint64_t));

	for (int i = 3; i < argc; i++) {
		if (file_queue == NULL) {
			file_queue = calloc(1, sizeof(files));
			queue_pointer = file_queue;
		} else {
			queue_pointer->next = calloc(1, sizeof(files));
			queue_pointer = queue_pointer->next;
		}
		queue_pointer->name = argv[i];
		queue_pointer->next = NULL;
	}

	queue_pointer = file_queue;

	coro_sched_init();

	for (int i = 0; i < pool_size; i++) {
		int* id = calloc(1, sizeof(int));
		*id = i;
		coro_new(coroutine_func_f, id);
	}

	struct coro *c;
	while ((c = coro_sched_wait()) != NULL) {
		coro_delete(c);
	}

	int cnt = argc - 3;
	int* cursors = calloc(cnt, sizeof(int));
	int **sorted = calloc(cnt, sizeof(int*));
	int *sorted_size = calloc(cnt, sizeof(int));
	
	queue_pointer = file_queue;
	cnt = 0;
	while (queue_pointer != NULL) {
		sorted[cnt] = queue_pointer->sorted;
		sorted_size[cnt] = queue_pointer->count;
		queue_pointer = queue_pointer->next;
		cnt++;
	}

	bool merged = false;

	FILE *output = fopen("output.txt", "w");

	while (!merged) {
		int min_ind = argc;
		int min = 0;

		for (int ind = 0; ind < cnt; ind++) {
			if (min_ind == argc && cursors[ind] < sorted_size[ind]) {
				min = sorted[ind][cursors[ind]];
				min_ind = ind;
			} else if (cursors[ind] < sorted_size[ind] && sorted[ind][cursors[ind]] < min) {
				min = sorted[ind][cursors[ind]];
				min_ind = ind;
			}
		}

		fprintf(output, "%d ", min);

		cursors[min_ind] += 1;

		merged = true;
		for (int ind = 0; ind < cnt; ind++) {
			merged = merged && cursors[ind] == sorted_size[ind];
		}
	}
	fclose(output);

	free(sorted);
	free(cursors);
	free(sorted_size);
	free(time_taken);
	free(last_yield);
	free(file_queue);

	t = get_current_time_in_microseconds() - t; 

	printf("Total time taken is %llu microseconds\n", t);

	return 0;
}
