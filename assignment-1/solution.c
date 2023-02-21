#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libcoro.h"

typedef struct files {
	const char *name;
	FILE *tmp;

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
    clock_gettime(CLOCK_REALTIME, &tp);
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

FILE* write_array_to_tmp_file(int *arr, int size) {
    FILE *fp = tmpfile();

    for (int i = 0; i < size; i++) {
        fprintf(fp, "%d", arr[i]);

        if (i != size - 1) {
            fprintf(fp, " ");
        }
    }

	rewind(fp);
	return fp;
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

		file->tmp = write_array_to_tmp_file(numbers, cnt);
	}

	time_taken[*id] += get_current_time_in_microseconds() - last_yield[*id];
	printf("coroutine %d finsihed with %lld switches and executing time %f seconds\n", *id, coro_switch_count(this), time_taken[*id] * 0.000001);

	free(context);
	return 0;
}

int main(int argc, char **argv) {
    long t = get_current_time_in_microseconds(); 

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

	FILE **tmp_files = calloc(argc - 3, sizeof(FILE *));

	queue_pointer = file_queue;
	int cnt = 0;
	while (queue_pointer != NULL) {
		tmp_files[cnt] = queue_pointer->tmp;
		queue_pointer = queue_pointer->next;
		cnt++;
	}

	int* cursors = calloc(cnt, sizeof(int));
	bool* file_processed = calloc(cnt, sizeof(bool));
	bool merged = false;

	for (int i = 0; i < cnt; i++) {
		file_processed[i] = false;
		if (fscanf(tmp_files[i], "%d", &cursors[i]) == EOF) {
			file_processed[i] = true;
			fclose(tmp_files[i]);
		}
	}

	FILE *output = fopen("output.txt", "w");

	while (!merged) {
		int min_ind = argc;
		int min = 0;

		for (int ind = 0; ind < cnt; ind++) {
			if (min_ind == argc && !file_processed[ind]) {
				min = cursors[ind];
				min_ind = ind;
			} else if (cursors[ind] < min && !file_processed[ind]) {
				min = cursors[ind];
				min_ind = ind;
			}
		}

		fprintf(output, "%d ", min);

		if (fscanf(tmp_files[min_ind], "%d", &cursors[min_ind]) == EOF) {
			file_processed[min_ind] = true;
			fclose(tmp_files[min_ind]);
		}

		merged = true;
		for (int ind = 0; ind < cnt; ind++) {
			merged = merged && file_processed[ind];
		}
	}
	fclose(output);

	t = get_current_time_in_microseconds() - t; 

	printf("Total time taken is %f seconds\n", t * 0.000001);

	return 0;
}
