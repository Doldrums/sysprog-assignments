#include "userfs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  BLOCK_SIZE = 512,
  MAX_FILE_SIZE = 1024 * 1024 * 1024,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
  /** Block index. */
  int index;
  /** Block memory. */
  char *memory;
  /** How many bytes are occupied. */
  int occupied;
  /** Next block in the file. */
  struct block *next;
  /** Previous block in the file. */
  struct block *prev;
};

struct file {
  /** Double-linked list of file blocks. */
  struct block *block_list;
  /**
   * Last block in the list above for fast access to the end
   * of file.
   */
  struct block *last_block;
  /** How many file descriptors are opened on the file. */
  int refs;
  /** File name. */
  char *name;
  /** Files are stored in a double-linked list. */
  struct file *next;
  struct file *prev;

  /** Is file marked for deletion when last description is closed */
  bool deleted;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
  /** Pointer to the file this file descriptor is associated with. */
  struct file *file;

  /** File access mode flags. */
  int flags;

  /** Pointer to the block in the file that this file descriptor is pointing
   * to.*/
  struct block *block;

  /** Byte offset within the block that this file descriptor is pointing to. */
  int offset;

  /* PUT HERE OTHER MEMBERS */
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code ufs_errno() { return ufs_error_code; }

int ufs_delete_file(struct file *f) {
  /* Remove the file from the file list. */
  if (f->prev != NULL) {
    f->prev->next = f->next;
  } else {
    file_list = f->next;
  }
  if (f->next != NULL) {
    f->next->prev = f->prev;
  }

  /* Free the file name. */
  free(f->name);

  /* Free the file blocks. */
  struct block *b = f->block_list;
  while (b != NULL) {
    struct block *next = b->next;
    free(b->memory);
    free(b);
    b = next;
  }

  /* Free the file itself. */
  free(f);

  return 0;
}

struct block *ufs_allocate_block() {
  struct block *new_block = malloc(sizeof(struct block));
  if (!new_block) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return NULL;
  }

  new_block->index = 0;
  new_block->memory = malloc(BLOCK_SIZE);
  new_block->occupied = 0;
  new_block->next = NULL;
  new_block->prev = NULL;

  return new_block;
}

int ufs_open(const char *filename, int flags) {
  /* Find an available slot in the file descriptors array. */
  int fd = -1;
  for (int i = 0; i < file_descriptor_count; i++) {
    if (file_descriptors[i] == NULL) {
      fd = i;
      break;
    }
  }

  /* If there's no available slot, allocate a new one. */
  if (fd == -1) {
    fd = file_descriptor_count++;
    if (fd >= file_descriptor_capacity) {
      /* Expand the file descriptors array. */
      int new_capacity = file_descriptor_capacity * 2;
      if (new_capacity == 0) {
        new_capacity = 1;
      }
      file_descriptors =
          realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
      if (file_descriptors == NULL) {
        /* Out of memory. */
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      memset(file_descriptors + file_descriptor_capacity, 0,
             (new_capacity - file_descriptor_capacity) *
                 sizeof(struct filedesc *));
      file_descriptor_capacity = new_capacity;
    }
  }

  /* Find the file in the file list. */
  struct file *f = file_list;
  while (f != NULL) {
    if (strcmp(f->name, filename) == 0 && !f->deleted) {
      break;
    }
    f = f->next;
  }

  /* If the file doesn't exist, create it if UFS_CREATE */
  if (f == NULL && (flags & UFS_CREATE) != UFS_CREATE) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  } else if (f == NULL) {
    f = malloc(sizeof(struct file));
    if (f == NULL) {
      /* Out of memory. */
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    f->name = strdup(filename);
    if (f->name == NULL) {
      /* Out of memory. */
      free(f);
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    f->block_list = NULL;
    f->last_block = NULL;
    f->refs = 0;
    f->next = file_list;
    f->prev = NULL;
    f->deleted = false;
    if (file_list != NULL) {
      file_list->prev = f;
    }
    file_list = f;
  }

  /* Allocate a file descriptor and fill it in. */
  struct filedesc *fdesc = malloc(sizeof(struct filedesc));
  if (fdesc == NULL) {
    /* Out of memory. */
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }
  fdesc->file = f;
  fdesc->offset = 0;
  fdesc->block = f->block_list;
  fdesc->flags = flags;
  file_descriptors[fd] = fdesc;

  /* Increase the reference count of the file. */
  f->refs++;

  return fd;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {
  /* Check if the file descriptor is valid. */
  if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  // Get the file descriptor from the file table.
  struct filedesc *filedesc = file_descriptors[fd];

  // Check if file is opened for writing.
  if (filedesc->flags & UFS_READ_ONLY) {
    ufs_error_code = UFS_ERR_NO_PERMISSION;
    return -1;
  }

  struct file *file = filedesc->file;

  // If the file is empty, create the first block
  if (file->block_list == NULL) {
    file->block_list = ufs_allocate_block();
    if (file->block_list == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    file->last_block = file->block_list;
  }

  struct block *block = filedesc->block;
  if (block == NULL) {
    block = filedesc->block = filedesc->file->block_list;
  }

  if (filedesc->block->index * BLOCK_SIZE + block->occupied + size >
      MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  ssize_t written_bytes = 0;
  size_t remaining_size = size;
  int offset = filedesc->offset;

  while (remaining_size > 0) {
    // If the current block is full, allocate a new block
    if (offset == BLOCK_SIZE) {
      block->next = ufs_allocate_block();
      if (block->next == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      block->next->index = block->index + 1;
      block->next->prev = block;
      if (block == file->last_block) {
        file->last_block = block->next;
      }
      block = block->next;
      offset = 0;
      filedesc->block = block;
    }

    // Calculate how many bytes can be written to the current block
    int writable_bytes = BLOCK_SIZE - offset;
    if (writable_bytes > remaining_size) {
      writable_bytes = remaining_size;
    }

    // Write the data to the current block
    memcpy(block->memory + offset, buf + written_bytes, writable_bytes);
    offset += writable_bytes;
    written_bytes += writable_bytes;
    remaining_size -= writable_bytes;
    // printf("writable_bytes: %d\n", offset);
    block->occupied = offset;
  }

  // Update the file descriptor's offset and block
  filedesc->offset = offset;

  return written_bytes;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
  /* Check if the file descriptor is valid. */
  if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  /* Get the file descriptor and the associated file. */
  struct filedesc *fdesc = file_descriptors[fd];

  // Check if file is opened for reading.
  if (fdesc->flags & UFS_WRITE_ONLY) {
    ufs_error_code = UFS_ERR_NO_PERMISSION;
    return -1;
  }

  // Read from the file block by block
  ssize_t bytes_read = 0;
  struct block *block = fdesc->block;
  if (block == NULL) {
    block = fdesc->block = fdesc->file->block_list;
  }
  int offset = fdesc->offset;
  while (block && bytes_read < size) {
    // Calculate how many bytes to read from this block
    int block_size = block->occupied - offset;
    if (block_size > size - bytes_read) {
      block_size = size - bytes_read;
    }

    // Copy the data from the block to the buffer
    memcpy(buf + bytes_read, block->memory + offset, block_size);
    bytes_read += block_size;
    fdesc->offset = offset + block_size;

    // Move to the next block if necessary
    if (bytes_read < size) {
      block = block->next;
      fdesc->block = block;
      offset = 0;
    }
  }

  // Return the number of bytes read
  return bytes_read;
}

int ufs_close(int fd) {
  /* Check if the file descriptor is valid. */
  if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  /* Get the file descriptor and the associated file. */
  struct filedesc *fdesc = file_descriptors[fd];
  struct file *f = fdesc->file;

  /* Decrease the reference count of the file. */
  f->refs--;

  /* If the reference count reaches zero, remove the file from the file list. */
  if (f->refs == 0 && f->deleted) {
    ufs_delete_file(f);
  }

  /* Free the file descriptor and set its corresponding entry in the file
   * descriptors array to NULL. */
  free(fdesc);
  file_descriptors[fd] = NULL;

  return 0;
}

int ufs_delete(const char *filename) {
  /* Find the file in the file list. */
  struct file *f = file_list;
  while (f != NULL) {
    if (strcmp(f->name, filename) == 0) {
      break;
    }
    f = f->next;
  }

  if (f == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  /* If there are open descriptors, mark the file as deleted. */
  if (f->refs > 0) {
    f->deleted = true;
  } else {
    ufs_delete_file(f);
  }

  return 0;
}

int ufs_resize(int fd, size_t new_size) {
  /* Check if the file descriptor is valid. */
  if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (new_size > MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  /* Get the file descriptor and the associated file. */
  struct filedesc *fdesc = file_descriptors[fd];
  struct file *f = fdesc->file;

  size_t file_size =
      f->last_block->index * BLOCK_SIZE + f->last_block->occupied;

  if (new_size > file_size) {
    ssize_t remaining_size = new_size - file_size;
    while (remaining_size > 0) {
      struct block *block = ufs_allocate_block();
      if (block == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      block->index = f->last_block->index + 1;
      f->last_block->next = block;
      block->prev = f->last_block;
      f->last_block = block;
      remaining_size -= BLOCK_SIZE;
    }
  } else {
    ssize_t remaining_size = file_size - new_size;
    while (remaining_size > BLOCK_SIZE) {
      struct block *block = f->last_block;
      f->last_block = block->prev;
      free(block->memory);
      free(block);
      remaining_size -= BLOCK_SIZE;
    }
    f->last_block->occupied = BLOCK_SIZE - remaining_size;

    for (int i = 0; i < file_descriptor_count; i++) {
      if (file_descriptors[i] != NULL && file_descriptors[i]->file == f &&
          file_descriptors[i]->block->index >= f->last_block->index) {
        file_descriptors[i]->block = f->last_block;
        file_descriptors[i]->offset = f->last_block->occupied;
      }
    }
  }

  return 0;
}
